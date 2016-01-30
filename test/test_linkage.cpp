#include "par_msquares.h"
#include "par_shapes.h"
#include "par_bluenoise.h"
#include "par_easycurl.h"
#include "par_filecache.h"
#include "par_bubbles.h"

int main(int argc, char* argv[])
{
    par_shapes_mesh* test = par_shapes_create_cylinder(10, 10);
    par_shapes_free_mesh(test);
    return 0;
}
