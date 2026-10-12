/* The kernel PNG decoder and its DEFLATE (drivers/png.c, drivers/inflate.c), compiled into libjt so ring 3 has one
   decoder, not a port. Music reads album art with it. */
#define PNG_USER 1
#include "../../drivers/inflate.c"
#include "../../drivers/png.c"
