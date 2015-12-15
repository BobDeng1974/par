// MSQUARES :: https://github.com/prideout/par
// Converts fp32 grayscale images, or 8-bit color images, into triangles.
//
// For grayscale images, a threshold is specified to determine insideness.
// For color images, an exact color is specified to determine insideness.
// Color images can be r8, rg16, rgb24, or rgba32. For a visual overview of
// the API and all the flags, see:
//
//     http://github.prideout.net/marching-squares/
//
// The MIT License
// Copyright (c) 2015 Philip Rideout

#include <stdint.h>

// -----------------------------------------------------------------------------
// BEGIN PUBLIC API
// -----------------------------------------------------------------------------

typedef uint8_t par_byte;

typedef struct par_msquares_meshlist_s par_msquares_meshlist;

// Encapsulates the results of a marching squares operation.
typedef struct {
    float* points;        // pointer to XY (or XYZ) vertex coordinates
    int npoints;          // number of vertex coordinates
    uint16_t* triangles;  // pointer to 3-tuples of vertex indices
    int ntriangles;       // number of 3-tuples
    int dim;              // number of floats per point (either 2 or 3)
    uint32_t color;       // used only with par_msquares_color_multi
    int nconntriangles;   // internal use only
    uint16_t* conntri;    // internal use only
} par_msquares_mesh;

// Reverses the "insideness" test.
#define PAR_MSQUARES_INVERT (1 << 0)

// Returns a meshlist with two meshes: one for the inside, one for the outside.
#define PAR_MSQUARES_DUAL (1 << 1)

// Returned meshes have 3-tuple coordinates instead of 2-tuples. When using
// from_color, the Z coordinate represents the alpha value of the color.  With
// from_grayscale, the Z coordinate represents the value of the nearest pixel in
// the source image.
#define PAR_MSQUARES_HEIGHTS (1 << 2)

// Applies a step function to the Z coordinates.  Requires HEIGHTS and DUAL.
#define PAR_MSQUARES_SNAP (1 << 3)

// Adds extrusion triangles to each mesh other than the lowest mesh.  Requires
// the PAR_MSQUARES_HEIGHTS flag to be present.
#define PAR_MSQUARES_CONNECT (1 << 4)

// Enables quick & dirty (not best) simpification of the returned mesh.
#define PAR_MSQUARES_SIMPLIFY (1 << 5)

par_msquares_meshlist* par_msquares_grayscale(float const* data, int width,
    int height, int cellsize, float threshold, int flags);

par_msquares_meshlist* par_msquares_color(par_byte const* data, int width,
    int height, int cellsize, uint32_t color, int bpp, int flags);

par_msquares_mesh const* par_msquares_get_mesh(par_msquares_meshlist*, int n);

int par_msquares_get_count(par_msquares_meshlist*);

void par_msquares_free(par_msquares_meshlist*);

typedef int (*par_msquares_inside_fn)(int, void*);
typedef float (*par_msquares_height_fn)(float, float, void*);

par_msquares_meshlist* par_msquares_function(int width, int height,
    int cellsize, int flags, void* context, par_msquares_inside_fn insidefn,
    par_msquares_height_fn heightfn);

par_msquares_meshlist* par_msquares_grayscale_multi(float const* data,
    int width, int height, int cellsize, float const* thresholds,
    int nthresholds, int flags);

par_msquares_meshlist* par_msquares_color_multi(par_byte const* data, int width,
    int height, int cellsize, int bpp, int flags);

// -----------------------------------------------------------------------------
// END PUBLIC API
// -----------------------------------------------------------------------------

#ifdef PAR_MSQUARES_IMPLEMENTATION

#include <stdlib.h>
#include <assert.h>
#include <float.h>
#include <string.h>

#define PAR_MIN(a, b) (a > b ? b : a)
#define PAR_MAX(a, b) (a > b ? a : b)
#define PAR_CLAMP(v, lo, hi) PAR_MAX(lo, PAR_MIN(hi, v))
#define PAR_ALLOC(T, N) ((T*) calloc(N * sizeof(T), 1))
#define PAR_SWAP(T, A, B) { T tmp = B; B = A; A = tmp; }

struct par_msquares_meshlist_s {
    int nmeshes;
    par_msquares_mesh** meshes;
};

static int** par_msquares_binary_point_table = 0;
static int** par_msquares_binary_triangle_table = 0;
static int* par_msquares_quaternary_triangle_table[64][4];
static int* par_msquares_quaternary_boundary_table[64][4];

static par_msquares_meshlist* par_msquares_merge(par_msquares_meshlist** lists,
    int count, int snap);

static void par_init_tables()
{
    char const* BINARY_TABLE =
        "0"
        "1017"
        "1123"
        "2023370"
        "1756"
        "2015560"
        "2123756"
        "3023035056"
        "1345"
        "4013034045057"
        "2124451"
        "3024045057"
        "2734467"
        "3013034046"
        "3124146167"
        "2024460";
    char const* binary_token = BINARY_TABLE;

    par_msquares_binary_point_table = PAR_ALLOC(int*, 16);
    par_msquares_binary_triangle_table = PAR_ALLOC(int*, 16);
    for (int i = 0; i < 16; i++) {
        int ntris = *binary_token - '0';
        binary_token++;
        int* sqrtris = par_msquares_binary_triangle_table[i] =
            PAR_ALLOC(int, (ntris + 1) * 3);
        sqrtris[0] = ntris;
        int mask = 0;
        int* sqrpts = par_msquares_binary_point_table[i] = PAR_ALLOC(int, 7);
        sqrpts[0] = 0;
        for (int j = 0; j < ntris * 3; j++, binary_token++) {
            int midp = *binary_token - '0';
            int bit = 1 << midp;
            if (!(mask & bit)) {
                mask |= bit;
                sqrpts[++sqrpts[0]] = midp;
            }
            sqrtris[j + 1] = midp;
        }
    }

    char const* QUATERNARY_TABLE =
        "2024046000"
        "3346360301112300"
        "3346360301112300"
        "3346360301112300"
        "3560502523013450"
        "2015056212414500"
        "4018087785756212313828348450"
        "4018087785756212313828348450"
        "3560502523013450"
        "4018087785756212313828348450"
        "2015056212414500"
        "4018087785756212313828348450"
        "3560502523013450"
        "4018087785756212313828348450"
        "4018087785756212313828348450"
        "2015056212414500"
        "3702724745001756"
        "2018087212313828348452785756"
        "4013034045057112301756"
        "4013034045057112301756"
        "2023037027347460"
        "1701312414616700"
        "2018087212313847857568348450"
        "2018087212313847857568348450"
        "4018087123138028348452785756"
        "1701467161262363513450"
        "2018087412313883484502785756"
        "2018087212313828348452785756"
        "4018087123138028348452785756"
        "1701467161262363513450"
        "2018087212313828348452785756"
        "2018087412313883484502785756"
        "3702724745001756"
        "4013034045057112301756"
        "2018087212313828348452785756"
        "4013034045057112301756"
        "4018087123138028348452785756"
        "2018087412313883484502785756"
        "1701467161262363513450"
        "2018087212313828348452785756"
        "2023037027347460"
        "2018087212313847857568348450"
        "1701312414616700"
        "2018087212313847857568348450"
        "4018087123138028348452785756"
        "2018087212313828348452785756"
        "1701467161262363513450"
        "2018087412313883484502785756"
        "3702724745001756"
        "4013034045057112301756"
        "4013034045057112301756"
        "2018087212313828348452785756"
        "4018087123138028348452785756"
        "2018087412313883484502785756"
        "2018087212313828348452785756"
        "1701467161262363513450"
        "4018087123138028348452785756"
        "2018087212313828348452785756"
        "2018087412313883484502785756"
        "1701467161262363513450"
        "2023037027347460"
        "2018087212313847857568348450"
        "2018087212313847857568348450"
        "1701312414616700";
    char const* quaternary_token = QUATERNARY_TABLE;

    int* quaternary_values = PAR_ALLOC(int, strlen(QUATERNARY_TABLE));
    int* vals = quaternary_values;
    for (int i = 0; i < 64; i++) {
        int ntris = *quaternary_token++ - '0';
        *vals = ntris;
        par_msquares_quaternary_triangle_table[i][0] = vals++;
        for (int j = 0; j < ntris * 3; j++) {
            int pt = *quaternary_token++ - '0';
            assert(pt >= 0 && pt < 9);
            *vals++ = pt;
        }
        ntris = *quaternary_token++ - '0';
        *vals = ntris;
        par_msquares_quaternary_triangle_table[i][1] = vals++;
        for (int j = 0; j < ntris * 3; j++) {
            int pt = *quaternary_token++ - '0';
            assert(pt >= 0 && pt < 9);
            *vals++ = pt;
        }
        ntris = *quaternary_token++ - '0';
        *vals = ntris;
        par_msquares_quaternary_triangle_table[i][2] = vals++;
        for (int j = 0; j < ntris * 3; j++) {
            int pt = *quaternary_token++ - '0';
            assert(pt >= 0 && pt < 9);
            *vals++ = pt;
        }
        ntris = *quaternary_token++ - '0';
        *vals = ntris;
        par_msquares_quaternary_triangle_table[i][3] = vals++;
        for (int j = 0; j < ntris * 3; j++) {
            int pt = *quaternary_token++ - '0';
            assert(pt >= 0 && pt < 9);
            *vals++ = pt;
        }
    }
    assert(vals == quaternary_values + strlen(QUATERNARY_TABLE));

    char const* QUATERNARY_EDGES =
        "0000"
        "21323100213231002132310023502530"
        "215251003185338135830318533813583023502530"
        "318533813583021525100318533813583023502530"
        "318533813583031853381358302152510025700275"
        "318733813583378541357231027541357231027523702730"
        "21727100318733813783031873381378303387035833785"
        "217471352530318735810378531873381358337853387035833785"
        "2174713525303187338135833785318735810378525700275"
        "41357231027531873381358337854135723102753387035833785"
        "3187358103785217471352530318733813583378523702730"
        "31873381378302172710031873381378303387035833785"
        "3187338135833785217471352530318735810378525700275"
        "41357231027541357231027531873381358337853387035833785"
        "318735810378531873381358337852174713525303387035833785"
        "3187338135833785318735810378521747135253023702730"
        "3187338137830318733813783021727100";
    quaternary_token = QUATERNARY_EDGES;

    quaternary_values = PAR_ALLOC(int, strlen(QUATERNARY_EDGES));
    vals = quaternary_values;
    for (int i = 0; i < 64; i++) {
        int nedges = *quaternary_token++ - '0';
        *vals = nedges;
        par_msquares_quaternary_boundary_table[i][0] = vals++;
        for (int j = 0; j < nedges; j++) {
            int pt = *quaternary_token++ - '0';
            assert(pt >= 0 && pt < 9);
            *vals++ = pt;
        }
        nedges = *quaternary_token++ - '0';
        *vals = nedges;
        par_msquares_quaternary_boundary_table[i][1] = vals++;
        for (int j = 0; j < nedges; j++) {
            int pt = *quaternary_token++ - '0';
            assert(pt >= 0 && pt < 9);
            *vals++ = pt;
        }
        nedges = *quaternary_token++ - '0';
        *vals = nedges;
        par_msquares_quaternary_boundary_table[i][2] = vals++;
        for (int j = 0; j < nedges; j++) {
            int pt = *quaternary_token++ - '0';
            assert(pt >= 0 && pt < 9);
            *vals++ = pt;
        }
        nedges = *quaternary_token++ - '0';
        *vals = nedges;
        par_msquares_quaternary_boundary_table[i][3] = vals++;
        for (int j = 0; j < nedges; j++) {
            int pt = *quaternary_token++ - '0';
            assert(pt >= 0 && pt < 9);
            *vals++ = pt;
        }
    }
    assert(vals == quaternary_values + strlen(QUATERNARY_EDGES));
}

typedef struct {
    float const* data;
    float threshold;
    float lower_bound;
    float upper_bound;
    int width;
    int height;
} par_gray_context;

static int gray_inside(int location, void* contextptr)
{
    par_gray_context* context = (par_gray_context*) contextptr;
    return context->data[location] > context->threshold;
}

static int gray_multi_inside(int location, void* contextptr)
{
    par_gray_context* context = (par_gray_context*) contextptr;
    float val = context->data[location];
    float upper = context->upper_bound;
    float lower = context->lower_bound;
    return val >= lower && val < upper;
}

static float gray_height(float x, float y, void* contextptr)
{
    par_gray_context* context = (par_gray_context*) contextptr;
    int i = PAR_CLAMP(context->width * x, 0, context->width - 1);
    int j = PAR_CLAMP(context->height * y, 0, context->height - 1);
    return context->data[i + j * context->width];
}

typedef struct {
    par_byte const* data;
    par_byte color[4];
    int bpp;
    int width;
    int height;
} par_color_context;

static int color_inside(int location, void* contextptr)
{
    par_color_context* context = (par_color_context*) contextptr;
    par_byte const* data = context->data + location * context->bpp;
    for (int i = 0; i < context->bpp; i++) {
        if (data[i] != context->color[i]) {
            return 0;
        }
    }
    return 1;
}

static float color_height(float x, float y, void* contextptr)
{
    par_color_context* context = (par_color_context*) contextptr;
    assert(context->bpp == 4);
    int i = PAR_CLAMP(context->width * x, 0, context->width - 1);
    int j = PAR_CLAMP(context->height * y, 0, context->height - 1);
    int k = i + j * context->width;
    return context->data[k * 4 + 3] / 255.0;
}

par_msquares_meshlist* par_msquares_color(par_byte const* data, int width,
    int height, int cellsize, uint32_t color, int bpp, int flags)
{
    par_color_context context;
    context.bpp = bpp;
    context.color[0] = (color >> 16) & 0xff;
    context.color[1] = (color >> 8) & 0xff;
    context.color[2] = (color & 0xff);
    context.color[3] = (color >> 24) & 0xff;
    context.data = data;
    context.width = width;
    context.height = height;
    return par_msquares_function(
        width, height, cellsize, flags, &context, color_inside, color_height);
}

par_msquares_meshlist* par_msquares_grayscale(float const* data, int width,
    int height, int cellsize, float threshold, int flags)
{
    par_gray_context context;
    context.width = width;
    context.height = height;
    context.data = data;
    context.threshold = threshold;
    return par_msquares_function(
        width, height, cellsize, flags, &context, gray_inside, gray_height);
}

par_msquares_meshlist* par_msquares_grayscale_multi(float const* data,
    int width, int height, int cellsize, float const* thresholds,
    int nthresholds, int flags)
{
    par_msquares_meshlist* mlists[2];
    mlists[0] = PAR_ALLOC(par_msquares_meshlist, 1);
    int connect = flags & PAR_MSQUARES_CONNECT;
    int snap = flags & PAR_MSQUARES_SNAP;
    int heights = flags & PAR_MSQUARES_HEIGHTS;
    if (!heights) {
        snap = connect = 0;
    }
    flags &= ~PAR_MSQUARES_INVERT;
    flags &= ~PAR_MSQUARES_DUAL;
    flags &= ~PAR_MSQUARES_CONNECT;
    flags &= ~PAR_MSQUARES_SNAP;
    par_gray_context context;
    context.width = width;
    context.height = height;
    context.data = data;
    context.lower_bound = -FLT_MAX;
    for (int i = 0; i <= nthresholds; i++) {
        int mergeconf = i > 0 ? connect : 0;
        if (i == nthresholds) {
            context.upper_bound = FLT_MAX;
            mergeconf |= snap;
        } else {
            context.upper_bound = thresholds[i];
        }
        mlists[1] = par_msquares_function(width, height, cellsize, flags,
            &context, gray_multi_inside, gray_height);
        mlists[0] = par_msquares_merge(mlists, 2, mergeconf);
        context.lower_bound = context.upper_bound;
        flags |= connect;
    }
    return mlists[0];
}

par_msquares_mesh const* par_msquares_get_mesh(
    par_msquares_meshlist* mlist, int mindex)
{
    assert(mlist && mindex < mlist->nmeshes);
    return mlist->meshes[mindex];
}

int par_msquares_get_count(par_msquares_meshlist* mlist)
{
    assert(mlist);
    return mlist->nmeshes;
}

void par_msquares_free(par_msquares_meshlist* mlist)
{
    if (!mlist) {
        return;
    }
    par_msquares_mesh** meshes = mlist->meshes;
    for (int i = 0; i < mlist->nmeshes; i++) {
        free(meshes[i]->points);
        free(meshes[i]->triangles);
        free(meshes[i]);
    }
    free(meshes);
    free(mlist);
}

// Combine multiple meshlists by moving mesh pointers, and optionally apply
// a "snap" operation that assigns a single Z value across all verts in each
// mesh.  The Z value determined by the mesh's position in the final mesh list.
static par_msquares_meshlist* par_msquares_merge(par_msquares_meshlist** lists,
    int count, int snap)
{
    par_msquares_meshlist* merged = PAR_ALLOC(par_msquares_meshlist, 1);
    merged->nmeshes = 0;
    for (int i = 0; i < count; i++) {
        merged->nmeshes += lists[i]->nmeshes;
    }
    merged->meshes = PAR_ALLOC(par_msquares_mesh*, merged->nmeshes);
    par_msquares_mesh** pmesh = merged->meshes;
    for (int i = 0; i < count; i++) {
        par_msquares_meshlist* meshlist = lists[i];
        for (int j = 0; j < meshlist->nmeshes; j++) {
            *pmesh++ = meshlist->meshes[j];
        }
        free(meshlist);
    }
    if (!snap) {
        return merged;
    }
    pmesh = merged->meshes;
    float zmin = FLT_MAX;
    float zmax = -zmin;
    for (int i = 0; i < merged->nmeshes; i++, pmesh++) {
        float* pzed = (*pmesh)->points + 2;
        for (int j = 0; j < (*pmesh)->npoints; j++, pzed += 3) {
            zmin = PAR_MIN(*pzed, zmin);
            zmax = PAR_MAX(*pzed, zmax);
        }
    }
    float zextent = zmax - zmin;
    pmesh = merged->meshes;
    for (int i = 0; i < merged->nmeshes; i++, pmesh++) {
        float* pzed = (*pmesh)->points + 2;
        float zed = zmin + zextent * i / (merged->nmeshes - 1);
        for (int j = 0; j < (*pmesh)->npoints; j++, pzed += 3) {
            *pzed = zed;
        }
    }
    if (!(snap & PAR_MSQUARES_CONNECT)) {
        return merged;
    }
    for (int i = 1; i < merged->nmeshes; i++) {
        par_msquares_mesh* mesh = merged->meshes[i];

        // Find all extrusion points.  This is tightly coupled to the
        // tessellation code, which generates two "connector" triangles for each
        // extruded edge.  The first two verts of the second triangle are the
        // verts that need to be displaced.
        char* markers = PAR_ALLOC(char, mesh->npoints);
        int tri = mesh->ntriangles - mesh->nconntriangles;
        while (tri < mesh->ntriangles) {
            markers[mesh->triangles[tri * 3 + 3]] = 1;
            markers[mesh->triangles[tri * 3 + 4]] = 1;
            tri += 2;
        }

        // Displace all extrusion points down to the previous level.
        float zed = zmin + zextent * (i - 1) / (merged->nmeshes - 1);
        float* pzed = mesh->points + 2;
        for (int j = 0; j < mesh->npoints; j++, pzed += 3) {
            if (markers[j]) {
                *pzed = zed;
            }
        }
        free(markers);
    }
    return merged;
}

par_msquares_meshlist* par_msquares_function(int width, int height,
    int cellsize, int flags, void* context, par_msquares_inside_fn insidefn,
    par_msquares_height_fn heightfn)
{
    assert(width > 0 && width % cellsize == 0);
    assert(height > 0 && height % cellsize == 0);

    if (flags & PAR_MSQUARES_DUAL) {
        int connect = flags & PAR_MSQUARES_CONNECT;
        int snap = flags & PAR_MSQUARES_SNAP;
        int heights = flags & PAR_MSQUARES_HEIGHTS;
        if (!heights) {
            snap = connect = 0;
        }
        flags ^= PAR_MSQUARES_INVERT;
        flags &= ~PAR_MSQUARES_DUAL;
        flags &= ~PAR_MSQUARES_CONNECT;
        par_msquares_meshlist* m[2];
        m[0] = par_msquares_function(width, height, cellsize, flags,
            context, insidefn, heightfn);
        flags ^= PAR_MSQUARES_INVERT;
        if (connect) {
            flags |= PAR_MSQUARES_CONNECT;
        }
        m[1] = par_msquares_function(width, height, cellsize, flags,
            context, insidefn, heightfn);
        return par_msquares_merge(m, 2, snap | connect);
    }

    int invert = flags & PAR_MSQUARES_INVERT;

    // Create the two code tables if we haven't already.  These are tables of
    // fixed constants, so it's embarassing that we use dynamic memory
    // allocation for them.  However it's easy and it's one-time-only.
    if (!par_msquares_binary_point_table) {
        par_init_tables();
    }

    // Allocate the meshlist and the first mesh.
    par_msquares_meshlist* mlist = PAR_ALLOC(par_msquares_meshlist, 1);
    mlist->nmeshes = 1;
    mlist->meshes = PAR_ALLOC(par_msquares_mesh*, 1);
    mlist->meshes[0] = PAR_ALLOC(par_msquares_mesh, 1);
    par_msquares_mesh* mesh = mlist->meshes[0];
    mesh->dim = (flags & PAR_MSQUARES_HEIGHTS) ? 3 : 2;
    int ncols = width / cellsize;
    int nrows = height / cellsize;

    // Worst case is four triangles and six verts per cell, so allocate that
    // much.
    int maxtris = ncols * nrows * 4;
    int maxpts = ncols * nrows * 6;
    int maxedges = ncols * nrows * 2;

    // However, if we include extrusion triangles for boundary edges,
    // we need space for another 4 triangles and 4 points per cell.
    uint16_t* conntris = 0;
    int nconntris = 0;
    uint16_t* edgemap = 0;
    if (flags & PAR_MSQUARES_CONNECT) {
        conntris = PAR_ALLOC(uint16_t, maxedges * 6);
        maxtris +=  maxedges * 2;
        maxpts += maxedges * 2;
        edgemap = PAR_ALLOC(uint16_t, maxpts);
        for (int i = 0; i < maxpts; i++) {
            edgemap[i] = 0xffff;
        }
    }
    uint16_t* tris = PAR_ALLOC(uint16_t, maxtris * 3);
    int ntris = 0;
    float* pts = PAR_ALLOC(float, maxpts * mesh->dim);
    int npts = 0;

    // The "verts" x/y/z arrays are the 4 corners and 4 midpoints around the
    // square, in counter-clockwise order.  The origin of "triangle space" is at
    // the lower-left, although we expect the image data to be in raster order
    // (starts at top-left).
    float vertsx[8], vertsy[8];
    float normalization = 1.0f / PAR_MAX(width, height);
    float normalized_cellsize = cellsize * normalization;
    int maxrow = (height - 1) * width;
    uint16_t* ptris = tris;
    uint16_t* pconntris = conntris;
    float* ppts = pts;
    uint8_t* prevrowmasks = PAR_ALLOC(uint8_t, ncols);
    int* prevrowinds = PAR_ALLOC(int, ncols * 3);

    // If simplification is enabled, we need to track all 'F' cells and their
    // repsective triangle indices.
    uint8_t* simplification_codes = 0;
    uint16_t* simplification_tris = 0;
    uint8_t* simplification_ntris = 0;
    if (flags & PAR_MSQUARES_SIMPLIFY) {
        simplification_codes = PAR_ALLOC(uint8_t, nrows * ncols);
        simplification_tris = PAR_ALLOC(uint16_t, nrows * ncols);
        simplification_ntris = PAR_ALLOC(uint8_t, nrows * ncols);
    }

    // Do the march!
    for (int row = 0; row < nrows; row++) {
        vertsx[0] = vertsx[6] = vertsx[7] = 0;
        vertsx[1] = vertsx[5] = 0.5 * normalized_cellsize;
        vertsx[2] = vertsx[3] = vertsx[4] = normalized_cellsize;
        vertsy[0] = vertsy[1] = vertsy[2] = normalized_cellsize * (row + 1);
        vertsy[4] = vertsy[5] = vertsy[6] = normalized_cellsize * row;
        vertsy[3] = vertsy[7] = normalized_cellsize * (row + 0.5);

        int northi = row * cellsize * width;
        int southi = PAR_MIN(northi + cellsize * width, maxrow);
        int northwest = invert ^ insidefn(northi, context);
        int southwest = invert ^ insidefn(southi, context);
        int previnds[8] = {0};
        uint8_t prevmask = 0;

        for (int col = 0; col < ncols; col++) {
            northi += cellsize;
            southi += cellsize;
            if (col == ncols - 1) {
                northi--;
                southi--;
            }

            int northeast = invert ^ insidefn(northi, context);
            int southeast = invert ^ insidefn(southi, context);
            int code = southwest | (southeast << 1) | (northwest << 2) |
                (northeast << 3);

            int const* pointspec = par_msquares_binary_point_table[code];
            int ptspeclength = *pointspec++;
            int currinds[8] = {0};
            uint8_t mask = 0;
            uint8_t prevrowmask = prevrowmasks[col];
            while (ptspeclength--) {
                int midp = *pointspec++;
                int bit = 1 << midp;
                mask |= bit;

                // The following six conditionals perform welding to reduce the
                // number of vertices.  The first three perform welding with the
                // cell to the west; the latter three perform welding with the
                // cell to the north.
                if (bit == 1 && (prevmask & 4)) {
                    currinds[midp] = previnds[2];
                    continue;
                }
                if (bit == 128 && (prevmask & 8)) {
                    currinds[midp] = previnds[3];
                    continue;
                }
                if (bit == 64 && (prevmask & 16)) {
                    currinds[midp] = previnds[4];
                    continue;
                }
                if (bit == 16 && (prevrowmask & 4)) {
                    currinds[midp] = prevrowinds[col * 3 + 2];
                    continue;
                }
                if (bit == 32 && (prevrowmask & 2)) {
                    currinds[midp] = prevrowinds[col * 3 + 1];
                    continue;
                }
                if (bit == 64 && (prevrowmask & 1)) {
                    currinds[midp] = prevrowinds[col * 3 + 0];
                    continue;
                }

                ppts[0] = vertsx[midp];
                ppts[1] = vertsy[midp];

                // Adjust the midpoints to a more exact crossing point.
                if (midp == 1) {
                    int begin = southi - cellsize / 2;
                    int previous = 0;
                    for (int i = 0; i < cellsize; i++) {
                        int offset = begin + i / 2 * ((i % 2) ? -1 : 1);
                        int inside = insidefn(offset, context);
                        if (i > 0 && inside != previous) {
                            ppts[0] = normalization *
                                (col * cellsize + offset - southi + cellsize);
                            break;
                        }
                        previous = inside;
                    }
                } else if (midp == 5) {
                    int begin = northi - cellsize / 2;
                    int previous = 0;
                    for (int i = 0; i < cellsize; i++) {
                        int offset = begin + i / 2 * ((i % 2) ? -1 : 1);
                        int inside = insidefn(offset, context);
                        if (i > 0 && inside != previous) {
                            ppts[0] = normalization *
                                (col * cellsize + offset - northi + cellsize);
                            break;
                        }
                        previous = inside;
                    }
                } else if (midp == 3) {
                    int begin = northi + width * cellsize / 2;
                    int previous = 0;
                    for (int i = 0; i < cellsize; i++) {
                        int offset = begin +
                            width * (i / 2 * ((i % 2) ? -1 : 1));
                        int inside = insidefn(offset, context);
                        if (i > 0 && inside != previous) {
                            ppts[1] = normalization *
                                (row * cellsize +
                                (offset - northi) / (float) width);
                            break;
                        }
                        previous = inside;
                    }
                } else if (midp == 7) {
                    int begin = northi + width * cellsize / 2 - cellsize;
                    int previous = 0;
                    for (int i = 0; i < cellsize; i++) {
                        int offset = begin +
                            width * (i / 2 * ((i % 2) ? -1 : 1));
                        int inside = insidefn(offset, context);
                        if (i > 0 && inside != previous) {
                            ppts[1] = normalization *
                                (row * cellsize +
                                (offset - northi - cellsize) / (float) width);
                            break;
                        }
                        previous = inside;
                    }
                }

                if (mesh->dim == 3) {
                    ppts[2] = heightfn(ppts[0], ppts[1], context);
                }

                ppts += mesh->dim;
                currinds[midp] = npts++;
            }

            int const* trianglespec = par_msquares_binary_triangle_table[code];
            int trispeclength = *trianglespec++;

            if (flags & PAR_MSQUARES_SIMPLIFY) {
                simplification_codes[ncols * row + col] = code;
                simplification_tris[ncols * row + col] = ntris;
                simplification_ntris[ncols * row + col] = trispeclength;
            }

            // Add triangles.
            while (trispeclength--) {
                int a = *trianglespec++;
                int b = *trianglespec++;
                int c = *trianglespec++;
                *ptris++ = currinds[c];
                *ptris++ = currinds[b];
                *ptris++ = currinds[a];
                ntris++;
            }

            // Create two extrusion triangles for each boundary edge.
            if (flags & PAR_MSQUARES_CONNECT) {
                trianglespec = par_msquares_binary_triangle_table[code];
                trispeclength = *trianglespec++;
                while (trispeclength--) {
                    int a = *trianglespec++;
                    int b = *trianglespec++;
                    int c = *trianglespec++;
                    int i = currinds[a];
                    int j = currinds[b];
                    int k = currinds[c];
                    int u = 0, v = 0, w = 0;
                    if ((a % 2) && (b % 2)) {
                        u = v = 1;
                    } else if ((a % 2) && (c % 2)) {
                        u = w = 1;
                    } else if ((b % 2) && (c % 2)) {
                        v = w = 1;
                    } else {
                        continue;
                    }
                    if (u && edgemap[i] == 0xffff) {
                        for (int d = 0; d < mesh->dim; d++) {
                            *ppts++ = pts[i * mesh->dim + d];
                        }
                        edgemap[i] = npts++;
                    }
                    if (v && edgemap[j] == 0xffff) {
                        for (int d = 0; d < mesh->dim; d++) {
                            *ppts++ = pts[j * mesh->dim + d];
                        }
                        edgemap[j] = npts++;
                    }
                    if (w && edgemap[k] == 0xffff) {
                        for (int d = 0; d < mesh->dim; d++) {
                            *ppts++ = pts[k * mesh->dim + d];
                        }
                        edgemap[k] = npts++;
                    }
                    if ((a % 2) && (b % 2)) {
                        *pconntris++ = i;
                        *pconntris++ = j;
                        *pconntris++ = edgemap[j];
                        *pconntris++ = edgemap[j];
                        *pconntris++ = edgemap[i];
                        *pconntris++ = i;
                    } else if ((a % 2) && (c % 2)) {
                        *pconntris++ = edgemap[k];
                        *pconntris++ = k;
                        *pconntris++ = i;
                        *pconntris++ = edgemap[i];
                        *pconntris++ = edgemap[k];
                        *pconntris++ = i;
                    } else if ((b % 2) && (c % 2)) {
                        *pconntris++ = j;
                        *pconntris++ = k;
                        *pconntris++ = edgemap[k];
                        *pconntris++ = edgemap[k];
                        *pconntris++ = edgemap[j];
                        *pconntris++ = j;
                    }
                    nconntris += 2;
                }
            }

            // Prepare for the next cell.
            prevrowmasks[col] = mask;
            prevrowinds[col * 3 + 0] = currinds[0];
            prevrowinds[col * 3 + 1] = currinds[1];
            prevrowinds[col * 3 + 2] = currinds[2];
            prevmask = mask;
            northwest = northeast;
            southwest = southeast;
            for (int i = 0; i < 8; i++) {
                previnds[i] = currinds[i];
                vertsx[i] += normalized_cellsize;
            }
        }
    }
    free(edgemap);
    free(prevrowmasks);
    free(prevrowinds);

    // Perform quick-n-dirty simplification by iterating two rows at a time.
    // In no way does this create the simplest possible mesh, but at least it's
    // fast and easy.
    if (flags & PAR_MSQUARES_SIMPLIFY) {
        int in_run = 0, start_run;

        // First figure out how many triangles we can eliminate.
        int neliminated_triangles = 0;
        for (int row = 0; row < nrows - 1; row += 2) {
            for (int col = 0; col < ncols; col++) {
                int a = simplification_codes[ncols * row + col] == 0xf;
                int b = simplification_codes[ncols * row + col + ncols] == 0xf;
                if (a && b) {
                    if (!in_run) {
                        in_run = 1;
                        start_run = col;
                    }
                    continue;
                }
                if (in_run) {
                    in_run = 0;
                    int run_width = col - start_run;
                    neliminated_triangles += run_width * 4 - 2;
                }
            }
            if (in_run) {
                in_run = 0;
                int run_width = ncols - start_run;
                neliminated_triangles += run_width * 4 - 2;
            }
        }

        // Build a new index array cell-by-cell.  If any given cell is 'F' and
        // its neighbor to the south is also 'F', then it's part of a run.
        int nnewtris = ntris + nconntris - neliminated_triangles;
        uint16_t* newtris = PAR_ALLOC(uint16_t, nnewtris * 3);
        uint16_t* pnewtris = newtris;
        in_run = 0;
        for (int row = 0; row < nrows - 1; row += 2) {
            for (int col = 0; col < ncols; col++) {
                int cell = ncols * row + col;
                int south = cell + ncols;
                int a = simplification_codes[cell] == 0xf;
                int b = simplification_codes[south] == 0xf;
                if (a && b) {
                    if (!in_run) {
                        in_run = 1;
                        start_run = col;
                    }
                    continue;
                }
                if (in_run) {
                    in_run = 0;
                    int nw_cell = ncols * row + start_run;
                    int ne_cell = ncols * row + col - 1;
                    int sw_cell = nw_cell + ncols;
                    int se_cell = ne_cell + ncols;
                    int nw_tri = simplification_tris[nw_cell];
                    int ne_tri = simplification_tris[ne_cell];
                    int sw_tri = simplification_tris[sw_cell];
                    int se_tri = simplification_tris[se_cell];
                    int nw_corner = nw_tri * 3 + 4;
                    int ne_corner = ne_tri * 3 + 0;
                    int sw_corner = sw_tri * 3 + 2;
                    int se_corner = se_tri * 3 + 1;
                    *pnewtris++ = tris[se_corner];
                    *pnewtris++ = tris[sw_corner];
                    *pnewtris++ = tris[nw_corner];
                    *pnewtris++ = tris[nw_corner];
                    *pnewtris++ = tris[ne_corner];
                    *pnewtris++ = tris[se_corner];
                }
                int ncelltris = simplification_ntris[cell];
                int celltri = simplification_tris[cell];
                for (int t = 0; t < ncelltris; t++, celltri++) {
                    *pnewtris++ = tris[celltri * 3];
                    *pnewtris++ = tris[celltri * 3 + 1];
                    *pnewtris++ = tris[celltri * 3 + 2];
                }
                ncelltris = simplification_ntris[south];
                celltri = simplification_tris[south];
                for (int t = 0; t < ncelltris; t++, celltri++) {
                    *pnewtris++ = tris[celltri * 3];
                    *pnewtris++ = tris[celltri * 3 + 1];
                    *pnewtris++ = tris[celltri * 3 + 2];
                }
            }
            if (in_run) {
                in_run = 0;
                int nw_cell = ncols * row + start_run;
                int ne_cell = ncols * row + ncols - 1;
                int sw_cell = nw_cell + ncols;
                int se_cell = ne_cell + ncols;
                int nw_tri = simplification_tris[nw_cell];
                int ne_tri = simplification_tris[ne_cell];
                int sw_tri = simplification_tris[sw_cell];
                int se_tri = simplification_tris[se_cell];
                int nw_corner = nw_tri * 3 + 4;
                int ne_corner = ne_tri * 3 + 0;
                int sw_corner = sw_tri * 3 + 2;
                int se_corner = se_tri * 3 + 1;
                *pnewtris++ = tris[se_corner];
                *pnewtris++ = tris[sw_corner];
                *pnewtris++ = tris[nw_corner];
                *pnewtris++ = tris[nw_corner];
                *pnewtris++ = tris[ne_corner];
                *pnewtris++ = tris[se_corner];
            }
        }
        ptris = pnewtris;
        ntris -= neliminated_triangles;
        free(tris);
        tris = newtris;
        free(simplification_codes);
        free(simplification_tris);
        free(simplification_ntris);

        // Remove unreferenced points.
        char* markers = PAR_ALLOC(char, npts);
        ptris = tris;
        int newnpts = 0;
        for (int i = 0; i < ntris * 3; i++, ptris++) {
            if (!markers[*ptris]) {
                newnpts++;
                markers[*ptris] = 1;
            }
        }
        for (int i = 0; i < nconntris * 3; i++) {
            if (!markers[conntris[i]]) {
                newnpts++;
                markers[conntris[i]] = 1;
            }
        }
        float* newpts = PAR_ALLOC(float, newnpts * mesh->dim);
        uint16_t* mapping = PAR_ALLOC(uint16_t, (ntris + nconntris) * 3);
        ppts = pts;
        float* pnewpts = newpts;
        int j = 0;
        if (mesh->dim == 3) {
            for (int i = 0; i < npts; i++, ppts += 3) {
                if (markers[i]) {
                    *pnewpts++ = ppts[0];
                    *pnewpts++ = ppts[1];
                    *pnewpts++ = ppts[2];
                    mapping[i] = j++;
                }
            }
        } else {
            for (int i = 0; i < npts; i++, ppts += 2) {
                if (markers[i]) {
                    *pnewpts++ = ppts[0];
                    *pnewpts++ = ppts[1];
                    mapping[i] = j++;
                }
            }
        }
        free(pts);
        free(markers);
        pts = newpts;
        npts = newnpts;
        for (int i = 0; i < ntris * 3; i++) {
            tris[i] = mapping[tris[i]];
        }
        for (int i = 0; i < nconntris * 3; i++) {
            conntris[i] = mapping[conntris[i]];
        }
        free(mapping);
    }

    // Append all extrusion triangles to the main triangle array.
    // We need them to be last so that they form a contiguous sequence.
    pconntris = conntris;
    for (int i = 0; i < nconntris; i++) {
        *ptris++ = *pconntris++;
        *ptris++ = *pconntris++;
        *ptris++ = *pconntris++;
        ntris++;
    }
    free(conntris);

    // Final cleanup and return.
    assert(npts <= maxpts);
    assert(ntris <= maxtris);
    mesh->npoints = npts;
    mesh->points = pts;
    mesh->ntriangles = ntris;
    mesh->triangles = tris;
    mesh->nconntriangles = nconntris;
    return mlist;
}

static int par_msquares_cmp(const void *a, const void *b)
{
    uint32_t arg1 = *((uint32_t const*) a);
    uint32_t arg2 = *((uint32_t const*) b);
    if (arg1 < arg2) return -1;
    if (arg1 > arg2) return 1;
    return 0;
}

typedef int (*par_msquares_code_fn)(int, int, int, int, void*);

static int par_msquares_multi_code(int sw, int se, int ne, int nw)
{
    int colors[4];
    int code[4];
    int ncols = 0;
    colors[ncols] = sw;
    code[0] = ncols++;
    if (se == sw) {
        code[1] = code[0];
    } else {
        colors[ncols] = se;
        code[1] = ncols++;
    }
    if (ne == se) {
        code[2] = code[1];
    } else if (ne == sw) {
        code[2] = code[0];
    } else {
        colors[ncols] = ne;
        code[2] = ncols++;
    }
    if (nw == ne) {
        code[3] = code[2];
    } else if (nw == se) {
        code[3] = code[1];
    } else if (nw == sw) {
        code[3] = code[0];
    } else {
        colors[ncols] = nw;
        code[3] = ncols++;
    }
    return code[0] | (code[1] << 2) | (code[2] << 4) | (code[3] << 6);
}

static uint32_t par_msquares_argb(par_byte const* pdata, int bpp)
{
    uint32_t color = 0;
    if (bpp == 4) {
        color |= pdata[2];
        color |= pdata[1] << 8;
        color |= pdata[0] << 16;
        color |= pdata[3] << 24;
        return color;
    }
    for (int j = 0; j < bpp; j++) {
        color <<= 8;
        color |= pdata[j];
    }
    return color;
}

static void par_msquares_internal_finalize(par_msquares_meshlist* mlist)
{
    if (mlist->nmeshes < 2 || mlist->meshes[1]->nconntriangles == 0) {
        return;
    }
    for (int m = 1; m < mlist->nmeshes; m++) {
        par_msquares_mesh* mesh = mlist->meshes[m];
        int ntris = mesh->ntriangles + mesh->nconntriangles;
        uint16_t* triangles = PAR_ALLOC(uint16_t, ntris * 3);
        uint16_t* dst = triangles;
        uint16_t const* src = mesh->triangles;
        for (int t = 0; t < mesh->ntriangles; t++) {
            *dst++ = *src++;
            *dst++ = *src++;
            *dst++ = *src++;
        }
        src = mesh->conntri;
        for (int t = 0; t < mesh->nconntriangles; t++) {
            *dst++ = *src++;
            *dst++ = *src++;
            *dst++ = *src++;
        }
        free(mesh->triangles);
        free(mesh->conntri);
        mesh->triangles = triangles;
        mesh->ntriangles = ntris;
        mesh->conntri = 0;
        mesh->nconntriangles = 0;
    }
}

par_msquares_meshlist* par_msquares_color_multi(par_byte const* data, int width,
    int height, int cellsize, int bpp, int flags)
{
    if (!par_msquares_binary_point_table) {
        par_init_tables();
    }
    const int ncols = width / cellsize;
    const int nrows = height / cellsize;
    const int maxrow = (height - 1) * width;
    const int ncells = ncols * nrows;
    const int dim = (flags & PAR_MSQUARES_HEIGHTS) ? 3 : 2;
    const int west_to_east[9] =   {  2, -1, -1, -1, -1, -1,  4,  3, -1 };
    const int north_to_south[9] = { -1, -1, -1, -1,  2,  1,  0, -1, -1 };
    assert(!(flags & PAR_MSQUARES_HEIGHTS) || bpp == 4);
    assert(bpp > 0 && bpp <= 4 && "Bytes per pixel must be 1, 2, 3, or 4.");
    assert(!(flags & PAR_MSQUARES_SNAP) &&
        "SNAP is not supported with color_multi");
    assert(!(flags & PAR_MSQUARES_INVERT) &&
        "INVERT is not supported with color_multi");
    assert(!(flags & PAR_MSQUARES_DUAL) &&
        "DUAL is not supported with color_multi");

    // Find all unique colors and ensure there are no more than 256 colors.
    uint32_t colors[256];
    int ncolors = 0;
    par_byte const* pdata = data;
    for (int i = 0; i < width * height; i++, pdata += bpp) {
        uint32_t color = par_msquares_argb(pdata, bpp);
        if (0 == bsearch(&color, colors, ncolors, 4, par_msquares_cmp)) {
            assert(ncolors < 256);
            colors[ncolors++] = color;
            qsort(colors, ncolors, sizeof(uint32_t), par_msquares_cmp);
        }
    }

    // Convert the color image to grayscale using the mapping table.
    par_byte* pixels = PAR_ALLOC(par_byte, width * height);
    pdata = data;
    for (int i = 0; i < width * height; i++, pdata += bpp) {
        uint32_t color = par_msquares_argb(pdata, bpp);
        void* result = bsearch(&color, colors, ncolors, 4, par_msquares_cmp);
        pixels[i] = (uint32_t*) result - &colors[0];
    }

    // Allocate 1 mesh for each color.
    par_msquares_meshlist* mlist = PAR_ALLOC(par_msquares_meshlist, 1);
    mlist->nmeshes = ncolors;
    mlist->meshes = PAR_ALLOC(par_msquares_mesh*, ncolors);
    par_msquares_mesh* mesh;
    int maxtris_per_cell = 6;
    int maxpts_per_cell = 9;
    if (flags & PAR_MSQUARES_CONNECT) {
        maxpts_per_cell += 6;
    }
    for (int i = 0; i < ncolors; i++) {
        mesh = mlist->meshes[i] = PAR_ALLOC(par_msquares_mesh, 1);
        mesh->color = colors[i];
        mesh->points = PAR_ALLOC(float, ncells * maxpts_per_cell * dim);
        mesh->triangles = PAR_ALLOC(uint16_t, ncells * maxtris_per_cell * 3);
        mesh->dim = dim;
        if (flags & PAR_MSQUARES_CONNECT) {
            mesh->conntri = PAR_ALLOC(uint16_t, ncells * 8 * 3);
        }
    }

    // The "verts" x/y/z arrays are the 4 corners and 4 midpoints around the
    // square, in counter-clockwise order, starting at the lower-left.  The
    // ninth vert is the center point.

    float vertsx[9], vertsy[9];
    float normalization = 1.0f / PAR_MAX(width, height);
    float normalized_cellsize = cellsize * normalization;
    uint8_t cella[256];
    uint8_t cellb[256];
    uint8_t* currcell = cella;
    uint8_t* prevcell = cellb;
    uint16_t inds0[256 * 9];
    uint16_t inds1[256 * 9];
    uint16_t* currinds = inds0;
    uint16_t* previnds = inds1;
    uint16_t* rowindsa = PAR_ALLOC(uint16_t, ncols * 3 * 256);
    uint8_t* rowcellsa = PAR_ALLOC(uint8_t, ncols * 256);
    uint16_t* rowindsb = PAR_ALLOC(uint16_t, ncols * 3 * 256);
    uint8_t* rowcellsb = PAR_ALLOC(uint8_t, ncols * 256);
    uint16_t* prevrowinds = rowindsa;
    uint16_t* currrowinds = rowindsb;
    uint8_t* prevrowcells = rowcellsa;
    uint8_t* currrowcells = rowcellsb;
    uint32_t* simplification_words = 0;
    if (flags & PAR_MSQUARES_SIMPLIFY) {
        simplification_words = PAR_ALLOC(uint32_t, 2 * nrows * ncols);
    }

    // Do the march!
    for (int row = 0; row < nrows; row++) {
        vertsx[0] = vertsx[6] = vertsx[7] = 0;
        vertsx[1] = vertsx[5] = vertsx[8] = 0.5 * normalized_cellsize;
        vertsx[2] = vertsx[3] = vertsx[4] = normalized_cellsize;
        vertsy[0] = vertsy[1] = vertsy[2] = normalized_cellsize * (row + 1);
        vertsy[4] = vertsy[5] = vertsy[6] = normalized_cellsize * row;
        vertsy[3] = vertsy[7] = vertsy[8] = normalized_cellsize * (row + 0.5);
        int northi = row * cellsize * width;
        int southi = PAR_MIN(northi + cellsize * width, maxrow);
        int nwval = pixels[northi];
        int swval = pixels[southi];
        memset(currrowcells, 0, ncols * 256);

        for (int col = 0; col < ncols; col++) {
            northi += cellsize;
            southi += cellsize;
            if (col == ncols - 1) {
                northi--;
                southi--;
            }

            // Obtain 8-bit code and grab the four corresopnding triangle lists.
            int neval = pixels[northi];
            int seval = pixels[southi];
            int code = par_msquares_multi_code(swval, seval, neval, nwval) >> 2;
            int const* trispecs[4] = {
                par_msquares_quaternary_triangle_table[code][0],
                par_msquares_quaternary_triangle_table[code][1],
                par_msquares_quaternary_triangle_table[code][2],
                par_msquares_quaternary_triangle_table[code][3]
            };
            int ntris[4] = {
                *trispecs[0]++,
                *trispecs[1]++,
                *trispecs[2]++,
                *trispecs[3]++
            };
            int const* edgespecs[4] = {
                par_msquares_quaternary_boundary_table[code][0],
                par_msquares_quaternary_boundary_table[code][1],
                par_msquares_quaternary_boundary_table[code][2],
                par_msquares_quaternary_boundary_table[code][3]
            };
            int nedges[4] = {
                *edgespecs[0]++,
                *edgespecs[1]++,
                *edgespecs[2]++,
                *edgespecs[3]++
            };

            // Push triangles and points into the four affected meshes.
            int vals[4] = { swval, seval, neval, nwval };
            for (int m = 0; m < ncolors; m++) {
                currcell[m] = 0;
            }
            uint32_t colors = 0;
            uint32_t counts = 0;
            for (int c = 0; c < 4; c++) {
                int color = vals[c];
                colors |= color << (8 * c);
                counts |= ntris[c] << (8 * c);
                par_msquares_mesh* mesh = mlist->meshes[color];
                int usedpts[9] = {0};
                uint16_t* pcurrinds = currinds + 9 * color;
                uint16_t const* pprevinds = previnds + 9 * color;
                uint16_t const* pprevrowinds =
                    prevrowinds + ncols * 3 * color + col * 3;
                uint8_t prevrowcell = prevrowcells[color * ncols + col];
                float* pdst = mesh->points + mesh->npoints * mesh->dim;
                int previndex, prevflag;
                for (int t = 0; t < ntris[c] * 3; t++) {
                    uint16_t index = trispecs[c][t];
                    if (usedpts[index]) {
                        continue;
                    }
                    usedpts[index] = 1;
                    if (index < 8) {
                        currcell[color] |= 1 << index;
                    }

                    // Vertical welding.
                    previndex = north_to_south[index];
                    prevflag = (previndex > -1) ? (1 << previndex) : 0;
                    if (row > 0 && (prevrowcell & prevflag)) {
                        pcurrinds[index] = pprevrowinds[previndex];
                        continue;
                    }

                    // Horizontal welding.
                    previndex = west_to_east[index];
                    prevflag = (previndex > -1) ? (1 << previndex) : 0;
                    if (col > 0 && (prevcell[color] & prevflag)) {
                        pcurrinds[index] = pprevinds[previndex];
                        continue;
                    }

                    // Insert brand new point.
                    float* vertex = pdst;
                    *pdst++ = vertsx[index];
                    *pdst++ = 1 - vertsy[index];
                    if (mesh->dim == 3) {
                        float height = (mesh->color >> 24) / 255.0;
                        *pdst++ = height;
                    }
                    pcurrinds[index] = mesh->npoints++;

                    // If this is a midpoint, nudge it to the intersection.
                    if (index == 1) {
                        int begin = southi - cellsize;
                        for (int i = 1; i < cellsize; i++) {
                            int val = pixels[begin + i];
                            if (val != pixels[begin]) {
                                vertex[0] = vertsx[0] + normalized_cellsize *
                                    (float) i / cellsize;
                                break;
                            }
                        }
                    } else if (index == 5) {
                        int begin = northi - cellsize;
                        for (int i = 1; i < cellsize; i++) {
                            int val = pixels[begin + i];
                            if (val != pixels[begin]) {
                                vertex[0] = vertsx[0] + normalized_cellsize *
                                    (float) i / cellsize;
                                break;
                           }
                        }
                    } else if (index == 7) {
                        int begin = northi - cellsize;
                        for (int i = 1; i < cellsize; i++) {
                            int val = pixels[begin + i * width];
                            if (val != pixels[begin]) {
                                vertex[1] = (1 - vertsy[6]) -
                                    normalized_cellsize * (float) i / cellsize;
                                break;
                            }
                        }
                    } else if (index == 3) {
                        int begin = northi;
                        for (int i = 1; i < cellsize; i++) {
                            int val = pixels[begin + i * width];
                            if (val != pixels[begin]) {
                                vertex[1] = (1 - vertsy[4]) -
                                    normalized_cellsize * (float) i / cellsize;
                                break;
                            }
                        }
                    }
                }

                // Stamp out the cell's triangle indices for this color.
                uint16_t* tdst = mesh->triangles + mesh->ntriangles * 3;
                mesh->ntriangles += ntris[c];
                for (int t = 0; t < ntris[c] * 3; t++) {
                    uint16_t index = trispecs[c][t];
                    *tdst++ = pcurrinds[index];
                }

                // Add extrusion points and connective triangles if requested.
                if (!(flags & PAR_MSQUARES_CONNECT) || color == 0) {
                    continue;
                }
                int minalpha = mesh->color >> 24;
                for (int idx = 0; idx < color - 1; idx++) {
                    par_msquares_mesh* lowest_mesh = mlist->meshes[idx];
                    minalpha = PAR_MIN(minalpha, lowest_mesh->color >> 24);
                }
                for (int e = 0; e < nedges[c]; e++) {
                    uint16_t index = edgespecs[c][e];
                    *pdst++ = vertsx[index];
                    *pdst++ = 1 - vertsy[index];
                    if (mesh->dim == 3) {
                        float height = minalpha / 255.0;
                        *pdst++ = height;
                    }
                    mesh->npoints++;
                    if (e > 0) {
                        uint16_t i0 = mesh->npoints - 1;
                        uint16_t i1 = mesh->npoints - 2;
                        uint16_t i2 = pcurrinds[edgespecs[c][e - 1]];
                        uint16_t i3 = pcurrinds[edgespecs[c][e]];
                        uint16_t* ptr = mesh->conntri +
                            mesh->nconntriangles * 3;
                        *ptr++ = i2; *ptr++ = i1; *ptr++ = i0;
                        *ptr++ = i0; *ptr++ = i3; *ptr++ = i2;
                        mesh->nconntriangles += 2;
                    }
                }
            }

            // Stash the bottom indices for each mesh in this cell to enable
            // vertical as-you-go welding.
            uint8_t* pcurrrowcells = currrowcells;
            uint16_t* pcurrrowinds = currrowinds;
            uint16_t const* pcurrinds = currinds;
            for (int color = 0; color < ncolors; color++) {
                pcurrrowcells[col] = currcell[color];
                pcurrrowcells += ncols;
                pcurrrowinds[col * 3 + 0] = pcurrinds[0];
                pcurrrowinds[col * 3 + 1] = pcurrinds[1];
                pcurrrowinds[col * 3 + 2] = pcurrinds[2];
                pcurrrowinds += ncols * 3;
                pcurrinds += 9;
            }

            // Stash some information later used by simplification.
            if (flags & PAR_MSQUARES_SIMPLIFY) {
                int cell = col + row * ncols;
                simplification_words[cell * 2] = colors;
                simplification_words[cell * 2 + 1] = counts;
            }

            // Advance the cursor.
            nwval = neval;
            swval = seval;
            for (int i = 0; i < 9; i++) {
                vertsx[i] += normalized_cellsize;
            }
            PAR_SWAP(uint8_t*, prevcell, currcell);
            PAR_SWAP(uint16_t*, previnds, currinds);
        }
        PAR_SWAP(uint8_t*, prevrowcells, currrowcells);
        PAR_SWAP(uint16_t*, prevrowinds, currrowinds);
    }
    assert(mesh->npoints <= ncells * maxpts_per_cell);
    assert(mesh->ntriangles <= ncells * maxtris_per_cell);
    free(prevrowinds);
    free(prevrowcells);
    free(pixels);
    if (!(flags & PAR_MSQUARES_SIMPLIFY)) {
        par_msquares_internal_finalize(mlist);
        return mlist;
    }

    uint8_t* simplification_blocks = PAR_ALLOC(uint8_t, nrows * ncols);
    uint32_t* simplification_tris = PAR_ALLOC(uint32_t, nrows * ncols);
    uint8_t* simplification_ntris = PAR_ALLOC(uint8_t, nrows * ncols);

    // Perform quick-n-dirty simplification by iterating two rows at a time.
    // In no way does this create the simplest possible mesh, but at least it's
    // fast and easy.
    for (int color = 0; color < ncolors; color++) {
        par_msquares_mesh* mesh = mlist->meshes[color];

        // Populate the per-mesh info grids.
        int ntris = 0;
        for (int row = 0; row < nrows; row++) {
            for (int col = 0; col < ncols; col++) {
                int cell = ncols * row + col;
                uint32_t colors = simplification_words[cell * 2];
                uint32_t counts = simplification_words[cell * 2 + 1];
                int ncelltris = 0;
                int ncorners = 0;
                if ((colors & 0xff) == color) {
                    ncelltris = counts & 0xff;
                    ncorners++;
                }
                if (((colors >> 8) & 0xff) == color) {
                    ncelltris += (counts >> 8) & 0xff;
                    ncorners++;
                }
                if (((colors >> 16) & 0xff) == color) {
                    ncelltris += (counts >> 16) & 0xff;
                    ncorners++;
                }
                if (((colors >> 24) & 0xff) == color) {
                    ncelltris += (counts >> 24) & 0xff;
                    ncorners++;
                }
                simplification_ntris[cell] = ncelltris;
                simplification_tris[cell] = ntris;
                simplification_blocks[cell] = ncorners == 4;
                ntris += ncelltris;
            }
        }

        // First figure out how many triangles we can eliminate.
        int in_run = 0, start_run;
        int neliminated_triangles = 0;
        for (int row = 0; row < nrows - 1; row += 2) {
            for (int col = 0; col < ncols; col++) {
                int cell = ncols * row + col;
                int a = simplification_blocks[cell];
                int b = simplification_blocks[cell + ncols];
                if (a && b) {
                    if (!in_run) {
                        in_run = 1;
                        start_run = col;
                    }
                    continue;
                }
                if (in_run) {
                    in_run = 0;
                    int run_width = col - start_run;
                    neliminated_triangles += run_width * 4 - 2;
                }
            }
            if (in_run) {
                in_run = 0;
                int run_width = ncols - start_run;
                neliminated_triangles += run_width * 4 - 2;
            }
        }
        if (neliminated_triangles == 0) {
            continue;
        }

        // Build a new index array cell-by-cell.  If any given cell is 'F' and
        // its neighbor to the south is also 'F', then it's part of a run.
        int nnewtris = mesh->ntriangles - neliminated_triangles;
        uint16_t* newtris = PAR_ALLOC(uint16_t, nnewtris * 3);
        uint16_t* pnewtris = newtris;
        in_run = 0;
        uint16_t* tris = mesh->triangles;
        for (int row = 0; row < nrows - 1; row += 2) {
            for (int col = 0; col < ncols; col++) {
                int cell = ncols * row + col;
                int south = cell + ncols;
                int a = simplification_blocks[cell];
                int b = simplification_blocks[south];
                if (a && b) {
                    if (!in_run) {
                        in_run = 1;
                        start_run = col;
                    }
                    continue;
                }
                if (in_run) {
                    in_run = 0;
                    int nw_cell = ncols * row + start_run;
                    int ne_cell = ncols * row + col - 1;
                    int sw_cell = nw_cell + ncols;
                    int se_cell = ne_cell + ncols;
                    int nw_tri = simplification_tris[nw_cell];
                    int ne_tri = simplification_tris[ne_cell];
                    int sw_tri = simplification_tris[sw_cell];
                    int se_tri = simplification_tris[se_cell];
                    int nw_corner = nw_tri * 3 + 5;
                    int ne_corner = ne_tri * 3 + 2;
                    int sw_corner = sw_tri * 3 + 0;
                    int se_corner = se_tri * 3 + 1;
                    *pnewtris++ = tris[nw_corner];
                    *pnewtris++ = tris[sw_corner];
                    *pnewtris++ = tris[se_corner];
                    *pnewtris++ = tris[se_corner];
                    *pnewtris++ = tris[ne_corner];
                    *pnewtris++ = tris[nw_corner];
                }
                int ncelltris = simplification_ntris[cell];
                int celltri = simplification_tris[cell];
                for (int t = 0; t < ncelltris; t++, celltri++) {
                    *pnewtris++ = tris[celltri * 3];
                    *pnewtris++ = tris[celltri * 3 + 1];
                    *pnewtris++ = tris[celltri * 3 + 2];
                }
                ncelltris = simplification_ntris[south];
                celltri = simplification_tris[south];
                for (int t = 0; t < ncelltris; t++, celltri++) {
                    *pnewtris++ = tris[celltri * 3];
                    *pnewtris++ = tris[celltri * 3 + 1];
                    *pnewtris++ = tris[celltri * 3 + 2];
                }
            }
            if (in_run) {
                in_run = 0;
                int nw_cell = ncols * row + start_run;
                int ne_cell = ncols * row + ncols - 1;
                int sw_cell = nw_cell + ncols;
                int se_cell = ne_cell + ncols;
                int nw_tri = simplification_tris[nw_cell];
                int ne_tri = simplification_tris[ne_cell];
                int sw_tri = simplification_tris[sw_cell];
                int se_tri = simplification_tris[se_cell];
                int nw_corner = nw_tri * 3 + 5;
                int ne_corner = ne_tri * 3 + 2;
                int sw_corner = sw_tri * 3 + 0;
                int se_corner = se_tri * 3 + 1;
                *pnewtris++ = tris[nw_corner];
                *pnewtris++ = tris[sw_corner];
                *pnewtris++ = tris[se_corner];
                *pnewtris++ = tris[se_corner];
                *pnewtris++ = tris[ne_corner];
                *pnewtris++ = tris[nw_corner];
            }
        }
        mesh->ntriangles -= neliminated_triangles;
        free(mesh->triangles);
        mesh->triangles = newtris;
    }

    free(simplification_blocks);
    free(simplification_ntris);
    free(simplification_tris);
    free(simplification_words);
    par_msquares_internal_finalize(mlist);
    return mlist;
}

#undef PAR_MIN
#undef PAR_MAX
#undef PAR_CLAMP
#undef PAR_ALLOC
#undef PAR_SWAP
#endif
