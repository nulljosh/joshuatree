#ifndef JT_DOCK_ANIM_H
#define JT_DOCK_ANIM_H

/* Advance the dock's subpixel position by elapsed PIT ticks. Kept separate
   so the ramp can be checked with a known tick sequence on slow CI hosts. */
static inline int dock_anim_advance(int pos, int target, unsigned int elapsed,
                                    int magnify, int subpixels, int duration){
    if (elapsed > (unsigned int)duration) elapsed = (unsigned int)duration;
    int delta = (int)elapsed * magnify * subpixels / duration;
    if (pos < target) { pos += delta; if (pos > target) pos = target; }
    else if (pos > target) { pos -= delta; if (pos < target) pos = target; }
    return pos;
}

#endif
