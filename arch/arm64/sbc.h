#ifndef ARM64_SBC_H
#define ARM64_SBC_H
/* SBC, the Bluetooth A2DP audio codec, encoder side (sbc.c). Fixed point only; no libc. One struct per stream: sbc_init once,
   then sbc_encode for every 128 stereo frames. The frames go out back to back, which is what ffmpeg reads as a raw .sbc stream. */
struct sbc_enc {
    unsigned fs;            /* sampling frequency code in the header: 2 for 44100 Hz, 3 for 48000 Hz */
    unsigned bitpool;       /* 2 to 58, set by sbc_init */
    int x[2][80];           /* analysis history per channel, newest sample first */
    int subband[16][2][8];  /* the subband samples of the frame being encoded: block, channel, subband */
};
void sbc_init(struct sbc_enc *e, unsigned rate, unsigned bitpool);
unsigned sbc_encode(struct sbc_enc *e, const short *pcm, unsigned char *out);   /* returns 12 + 2 * bitpool bytes, at most 128 */
#endif
