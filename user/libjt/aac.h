/* AAC-LC decoder for Blackcomb's video (13.4.0): the sound in an MP4 (YouTube's 360p, most web video). Mono or stereo,
   long and short windows, sine and KBD windows, M/S and intensity stereo, TNS, pulses and noise bands. No SBR or PS
   (HE-AAC): their base layer still decodes, at half the rate. Floating point: a kernel build compiles it like ttf.c. */
#ifndef JT_AAC_H
#define JT_AAC_H

struct aac {
    int rate, channels, srix;
    float overlap[2][1024];
    int prev_shape[2];
    float spec[2][1024];
    const char *err;
};
/* asc: the MP4's AudioSpecificConfig. 0 ok, -1 not AAC-LC. */
int aac_init(struct aac *a, const unsigned char *asc, unsigned len);
/* One raw AAC frame to 1024 samples per channel, interleaved 16-bit, into out (2048 shorts for stereo). 1024, or -1. */
int aac_frame(struct aac *a, const unsigned char *data, unsigned len, short *out);
#endif
