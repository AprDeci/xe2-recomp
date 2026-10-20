/* VP9 frame headers, written from their fields (VP9 bitstream specification, 6.2 and 6.3).
 *
 * The video decode engine is handed a frame's fields and probabilities already parsed, with
 * only the tile data as a bitstream; a software decoder wants the whole frame. These rebuild
 * the uncompressed header from the fields and the compressed header as the probability
 * updates that take a context from one set of probabilities to another. */
#pragma once
#include <stdint.h>

/* A frame context: every adaptive probability, in the specification's coding order. */
typedef struct Vp9Probs {
    uint8_t tx8[2][1], tx16[2][2], tx32[2][3];
    uint8_t coef[4][2][2][6][6][3];   /* tx size, plane type, reference, band, context, node */
    uint8_t skip[3];
    uint8_t inter_mode[7][3];
    uint8_t interp_filter[4][2];
    uint8_t is_inter[4];
    uint8_t comp_mode[5];
    uint8_t single_ref[5][2];
    uint8_t comp_ref[5];
    uint8_t y_mode[4][9];
    uint8_t uv_mode[10][9];           /* (never updated by a header: adaptation only) */
    uint8_t partition[16][3];
    uint8_t mv_joint[3];
    uint8_t mv_sign[2], mv_class[2][10], mv_class0_bit[2], mv_bits[2][10];
    uint8_t mv_class0_fr[2][2][3], mv_fr[2][3], mv_class0_hp[2], mv_hp[2];
} Vp9Probs;

enum { VP9_ONLY_4X4, VP9_ALLOW_8X8, VP9_ALLOW_16X16, VP9_ALLOW_32X32, VP9_TX_MODE_SELECT };
enum { VP9_SINGLE_REFERENCE, VP9_COMPOUND_REFERENCE, VP9_REFERENCE_MODE_SELECT };
enum { VP9_EIGHTTAP, VP9_EIGHTTAP_SMOOTH, VP9_EIGHTTAP_SHARP, VP9_BILINEAR, VP9_SWITCHABLE };

typedef struct Vp9Frame {
    uint8_t profile, bit_depth;                     /* bit_depth: 8, 10, 12 */
    uint8_t show_existing, existing_idx;
    uint8_t key, show, error_resilient, intra_only, reset_context;
    uint8_t color_space, color_range, subsampling_x, subsampling_y;
    uint8_t refresh_flags;
    uint8_t ref_idx[3], sign_bias[3];               /* last, golden, altref */
    uint8_t found_ref;                              /* 1-3: size taken from that reference; 0: written */
    uint16_t width, height, render_width, render_height;
    uint8_t allow_hp, interp_filter;
    uint8_t refresh_context, parallel, context_idx;
    uint8_t lf_level, lf_sharpness, lf_delta_enabled, lf_delta_update;
    uint8_t lf_ref_update[4], lf_mode_update[2];
    int8_t lf_ref[4], lf_mode[2];
    uint8_t base_q;
    int8_t dq_y_dc, dq_uv_dc, dq_uv_ac;
    uint8_t seg_enabled, seg_update_map, seg_temporal, seg_update_data, seg_abs;
    uint8_t seg_tree[7], seg_pred[3];               /* 255: not coded */
    uint8_t seg_on[8][4];
    int16_t seg_data[8][4];
    uint8_t tile_cols_log2, tile_rows_log2;
    uint8_t tx_mode, reference_mode;                /* (in the compressed header) */
} Vp9Frame;

/* The uncompressed header, compressed_size included; returns its size in bytes. */
uint32_t vp9_write_uncompressed(const Vp9Frame *f, uint32_t compressed_size, uint8_t *out, uint32_t cap);
/* The compressed header taking `from` towards `to`: only the probabilities this frame codes
 * change, and motion vector ones only to odd values. *result (if given) gets what a decoder
 * holds afterwards. Returns the header's size. */
uint32_t vp9_write_compressed(const Vp9Frame *f, const Vp9Probs *from, const Vp9Probs *to, Vp9Probs *result, uint8_t *out, uint32_t cap);
/* The probabilities a context starts from (after a key frame or a reset). */
void vp9_default_probs(Vp9Probs *p);
/* How well context c serves as the base for `to` (higher is better): motion vector
 * probabilities can only be set to odd values, so a context they cannot reach is no good. */
int vp9_context_score(const Vp9Probs *c, const Vp9Probs *to);
/* A decoder's 8 reference slots, kept by the surfaces (buffers) the frames in them were decoded
 * to - all a hardware decoder is told about references. */
typedef struct Vp9Slots { uint64_t surface[8]; uint32_t age[8], clock; } Vp9Slots;
/* For a frame decoded to `current` that references refs[] (last, golden, altref): sets
 * f->ref_idx and f->refresh_flags (all slots for a key frame) and returns the slot it goes to. */
int vp9_assign_slots(Vp9Slots *s, Vp9Frame *f, const uint64_t refs[3], uint64_t current);

/* Delta index d for which inv_remap_prob(d, old) == new (new != old). */
int vp9_remap_prob(int new_prob, int old_prob);
int vp9_inv_remap_prob(int delta, int old_prob);
