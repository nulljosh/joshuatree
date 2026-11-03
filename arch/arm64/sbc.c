/* SBC, the mandatory codec of Bluetooth A2DP (A2DP spec, appendix B), encoded in fixed point so the Pi can stream sound to
   headphones later. sbc_encode takes 128 stereo frames of signed 16-bit PCM (left then right, interleaved) and writes one SBC
   frame: a 4-byte header with a CRC-8, sixteen 4-bit scale factors, then the quantized subband samples packed into the bitpool.
   Fixed choices: 16 blocks and 8 subbands a frame (128 samples a channel, 2.9 ms at 44.1 kHz); STEREO mode, so the channels are
   coded separately and JOINT_STEREO is not used; LOUDNESS bit allocation. The bitpool is set by sbc_init and clamped to 2..58, so
   a frame is 12 + 2 * bitpool bytes (82 at the default 35) and always fits the 128-byte buffer. Any rate other than 48000 is
   coded as 44100.
   Everything is integer. The analysis window and the cosine matrix are the spec's tables as integers (below), the accumulators
   are 64-bit, and the scale factors and quantizer invert the arithmetic an SBC decoder does, so a decoder gets back what was
   encoded. tools/checks/sbc-encode-check.py decodes the output with ffmpeg and checks the tone that comes back. */
#include "sbc.h"

#define SYNCWORD 0x9C
#define BLOCKS 16
#define SUBBANDS 8
#define SCALE_OUT_BITS 15    /* subband samples carry 15 fraction bits, the scale a decoder expects */
#define MIN_BITPOOL 2
#define MAX_BITPOOL 58       /* 12 + 2 * 58 = 128 bytes, the most a frame may take */
#define DEFAULT_BITPOOL 35

/* The analysis prototype window C[0..79] of the spec (section 12.8), as C * 2^16 so the windowed sum shifts down by 16. */
static const short proto8[80] = {
    0, 10, 22, 36, -54, 75, 97, 117,
    132, 138, 131, 106, 59, 12, 108, 229,
    371, 526, 685, 835, -960, 1042, 1063, 1004,
    848, 580, 192, -322, -959, 1711, 2561, 3486,
    4456, 5438, 6395, 7287, -8078, 8734, 9224, 9528,
    9631, 9528, 9224, 8734, 8078, -7287, -6395, -5438,
    -4456, -3486, -2561, -1711, 959, -322, 192, 580,
    848, 1004, 1063, 1042, 960, -835, -685, -526,
    -371, -229, -108, -12, -59, 106, 131, 138,
    132, 117, 97, 75, 54, -36, -22, -10,
};

/* The cosine matrix of the spec: row k is subband k, column j is cos((k + 0.5)(j - 4) pi / 8), times 2^15. */
static const short cos8[8 * 16] = {
    23170, 27246, 30274, 32138, -32768, 32138, 30274, 27246, 23170, 18205, 12540, 6393, 0, 6393, 12540, 18205,
    -23170, -6393, 12540, 27246, -32768, 27246, 12540, -6393, -23170, -32138, -30274, -18205, 0, -18205, -30274, -32138,
    -23170, -32138, -12540, 18205, -32768, 18205, -12540, -32138, -23170, 6393, 30274, 27246, 0, 27246, 30274, 6393,
    23170, -18205, -30274, 6393, -32768, 6393, -30274, -18205, 23170, 27246, -12540, -32138, 0, -32138, -12540, 27246,
    23170, 18205, -30274, -6393, -32768, -6393, -30274, 18205, 23170, -27246, -12540, 32138, 0, 32138, -12540, -27246,
    -23170, 32138, -12540, -18205, -32768, -18205, -12540, 32138, -23170, -6393, 30274, -27246, 0, -27246, 30274, -6393,
    -23170, 6393, 12540, -27246, -32768, -27246, 12540, 6393, -23170, 32138, -30274, 18205, 0, 18205, -30274, 32138,
    23170, -27246, 30274, -32138, -32768, -32138, 30274, -27246, 23170, -18205, 12540, -6393, 0, -6393, 12540, -18205,
};

/* Loudness offsets of the spec (appendix B, the 8-subband table), by sampling frequency code and subband. */
static const int offset8[4][8] = {
    { -2, 0, 0, 0, 0, 0, 0, 1 },
    { -3, 0, 0, 0, 0, 0, 1, 2 },
    { -4, 0, 0, 0, 0, 0, 1, 2 },
    { -4, 0, 0, 0, 0, 0, 1, 2 },
};

/* CRC-8 of the spec: polynomial 0x1D, starting value 0x0F, bytes most significant bit first. */
static unsigned char crc8(const unsigned char *data, unsigned n)
{
    unsigned crc = 0x0F;
    for (unsigned i = 0; i < n; i++) {
        crc ^= data[i];
        for (int b = 0; b < 8; b++)
            crc = (crc & 0x80) ? ((crc << 1) ^ 0x1D) & 0xFF : (crc << 1) & 0xFF;
    }
    return (unsigned char)crc;
}

/* One block of analysis: x[0] is the newest sample of the 80 in the window. Fold the window to 16 values, then the cosine
   transform gives the 8 subband samples. */
static void analyse(const int x[80], int out[8])
{
    long long y[16];
    for (int j = 0; j < 16; j++) {
        long long acc = 1LL << 15;   /* rounding for the shift by 16 */
        for (int m = 0; m < 5; m++)
            acc += (long long)proto8[j + 16 * m] * x[j + 16 * m];
        y[j] = acc >> 16;
    }
    for (int k = 0; k < 8; k++) {
        long long acc = 0;
        for (int j = 0; j < 16; j++)
            acc += (long long)cos8[k * 16 + j] * y[j];
        out[k] = (int)acc;
    }
}

/* Bit allocation of the spec, STEREO branch with LOUDNESS: the bitpool is spread over both channels by how much each subband
   needs, the most-needing subbands first, one bit at a time, until every bit is placed. */
static void allocate(unsigned fs, int bitpool, const int sf[2][8], int bits[2][8])
{
    int need[2][8], maxneed = 0, count = 0, slices = 0, slice;
    for (int ch = 0; ch < 2; ch++)
        for (int s = 0; s < SUBBANDS; s++) {
            if (sf[ch][s] == 0) {
                need[ch][s] = -5;
            } else {
                int loud = sf[ch][s] - offset8[fs][s];
                need[ch][s] = loud > 0 ? loud / 2 : loud;
            }
            if (need[ch][s] > maxneed)
                maxneed = need[ch][s];
            bits[ch][s] = 0;
        }

    slice = maxneed + 1;
    do {
        slice--;
        count += slices;
        slices = 0;
        for (int ch = 0; ch < 2; ch++)
            for (int s = 0; s < SUBBANDS; s++) {
                if (need[ch][s] > slice + 1 && need[ch][s] < slice + 16)
                    slices++;
                else if (need[ch][s] == slice + 1)
                    slices += 2;
            }
    } while (count + slices < bitpool);
    if (count + slices == bitpool) {
        count += slices;
        slice--;
    }

    for (int ch = 0; ch < 2; ch++)
        for (int s = 0; s < SUBBANDS; s++) {
            if (need[ch][s] < slice + 2) {
                bits[ch][s] = 0;
            } else {
                bits[ch][s] = need[ch][s] - slice;
                if (bits[ch][s] > 16)
                    bits[ch][s] = 16;
            }
        }

    int ch = 0, s = 0;
    while (count < bitpool) {
        if (bits[ch][s] >= 2 && bits[ch][s] < 16) {
            bits[ch][s]++;
            count++;
        } else if (need[ch][s] == slice + 1 && bitpool > count + 1) {
            bits[ch][s] = 2;
            count += 2;
        }
        if (ch == 1) {
            ch = 0;
            if (++s >= SUBBANDS)
                break;
        } else {
            ch = 1;
        }
    }

    ch = 0;
    s = 0;
    while (count < bitpool) {
        if (bits[ch][s] < 16) {
            bits[ch][s]++;
            count++;
        }
        if (ch == 1) {
            ch = 0;
            if (++s >= SUBBANDS)
                break;
        } else {
            ch = 1;
        }
    }
}

/* Write the low n bits of v, most significant first, at bit position *pos of out. Bits past the end of the frame are dropped. */
static void put_bits(unsigned char *out, unsigned *pos, unsigned limit, unsigned v, unsigned n)
{
    for (int i = (int)n - 1; i >= 0; i--, (*pos)++)
        if (*pos < limit && ((v >> i) & 1))
            out[*pos >> 3] |= (unsigned char)(0x80 >> (*pos & 7));
}

/* Position of the highest set bit of x (x is never 0 here). */
static int top_bit(unsigned long x)
{
    int p = 0;
    while (x >>= 1)
        p++;
    return p;
}

void sbc_init(struct sbc_enc *e, unsigned rate, unsigned bitpool)
{
    e->fs = rate == 48000 ? 3 : 2;
    e->bitpool = bitpool < MIN_BITPOOL ? MIN_BITPOOL : bitpool > MAX_BITPOOL ? MAX_BITPOOL : bitpool;
    for (int ch = 0; ch < 2; ch++)
        for (int i = 0; i < 80; i++)
            e->x[ch][i] = 0;
}

unsigned sbc_encode(struct sbc_enc *e, const short *pcm, unsigned char *out)
{
    unsigned frame_len = 12 + 2 * e->bitpool;
    unsigned limit = frame_len * 8;

    /* Analysis: each block takes 8 new samples per channel into the window, then gives its 8 subband samples. */
    for (int b = 0; b < BLOCKS; b++)
        for (int ch = 0; ch < 2; ch++) {
            int *x = e->x[ch];
            for (int i = 79; i >= 8; i--)
                x[i] = x[i - 8];
            for (int i = 0; i < 8; i++)
                x[i] = pcm[(b * 8 + 7 - i) * 2 + ch];
            analyse(x, e->subband[b][ch]);
        }

    /* Scale factor: the smallest f with every subband sample of the channel inside +-2^(f + 15 + 1). */
    int sf[2][8];
    for (int ch = 0; ch < 2; ch++)
        for (int s = 0; s < SUBBANDS; s++) {
            unsigned long x = 1UL << SCALE_OUT_BITS;
            for (int b = 0; b < BLOCKS; b++) {
                int t = e->subband[b][ch][s];
                if (t < 0)
                    t = -t;
                if (t != 0)
                    x |= (unsigned long)(t - 1);
            }
            int f = top_bit(x) - SCALE_OUT_BITS;
            sf[ch][s] = f > 15 ? 15 : f;
        }

    int bits[2][8];
    allocate(e->fs, (int)e->bitpool, sf, bits);

    /* Header: sync word, sampling frequency, 16 blocks, STEREO, LOUDNESS, 8 subbands, bitpool, CRC. */
    for (unsigned i = 0; i < frame_len; i++)
        out[i] = 0;
    out[0] = SYNCWORD;
    /* The allocation bit is 0 for LOUDNESS and 1 for SNR, so the loudness frames here carry a 0 there. */
    out[1] = (unsigned char)((e->fs << 6) | (3u << 4) | (2u << 2) | (0u << 1) | 1u);
    out[2] = (unsigned char)e->bitpool;

    unsigned pos = 32;
    for (int ch = 0; ch < 2; ch++)
        for (int s = 0; s < SUBBANDS; s++)
            put_bits(out, &pos, limit, (unsigned)sf[ch][s], 4);

    /* The CRC covers the two config bytes and the 16 scale factors, which sit in bytes 4 to 11. */
    unsigned char crc_in[10] = { out[1], out[2], out[4], out[5], out[6], out[7], out[8], out[9], out[10], out[11] };
    out[3] = crc8(crc_in, 10);

    /* Quantize each subband sample to its bits. The decoder reads level q back as the middle of its cell, so q is the
       position of the sample in [-D, D) cut into 2^bits - 1 equal cells. */
    pos = 96;
    for (int b = 0; b < BLOCKS; b++)
        for (int ch = 0; ch < 2; ch++)
            for (int s = 0; s < SUBBANDS; s++) {
                int n = bits[ch][s];
                if (n == 0)
                    continue;
                long long D = 1LL << (sf[ch][s] + SCALE_OUT_BITS + 1);
                long long L = (1LL << n) - 1;
                long long q = (L * (e->subband[b][ch][s] + D)) >> (sf[ch][s] + SCALE_OUT_BITS + 2);
                if (q < 0)
                    q = 0;
                if (q > L - 1)
                    q = L - 1;
                put_bits(out, &pos, limit, (unsigned)q, (unsigned)n);
            }

    return frame_len;
}
