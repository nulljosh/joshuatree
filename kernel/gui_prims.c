#include "gui_prims.h"

double gui_line_sqrt(double x){ double r; __asm__ volatile ("fsqrt" : "=t"(r) : "0"(x)); return r; }
