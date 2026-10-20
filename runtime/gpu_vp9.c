/* VP9 frame headers, written from their fields - see gpu_vp9.h. Section numbers are the VP9
 * bitstream specification's. */
#include <stddef.h>
#include <string.h>

#include "gpu_vp9.h"
#include "gpu_vp9_tables.h"

_Static_assert(sizeof(Vp9Probs) == sizeof vp9_default_bytes, "Vp9Probs layout");

void vp9_default_probs(Vp9Probs *p) { memcpy(p, vp9_default_bytes, sizeof *p); }

int vp9_assign_slots(Vp9Slots *s, Vp9Frame *f, const uint64_t refs[3], uint64_t current) {
    int slot = -1;
    if (f->key) {
        for (int i = 0; i < 8; i++) s->surface[i] = current;
        f->refresh_flags = 0xFF;
        slot = 0;
    } else {
        for (int i = 0; i < 3; i++) {   /* each reference: the first slot holding its surface */
            f->ref_idx[i] = 0;
            for (int k = 0; k < 8; k++) if (s->surface[k] == refs[i]) { f->ref_idx[i] = (uint8_t)k; break; }
        }
        for (int k = 0; k < 8 && slot < 0; k++) if (s->surface[k] == current) slot = k;   /* its surface, reused */
        if (slot < 0) {   /* else the longest unused slot whose frame no reference needs (or that holds a copy) */
            uint32_t best = ~0u;
            for (int k = 0; k < 8; k++) {
                int needed = k == f->ref_idx[0] || k == f->ref_idx[1] || k == f->ref_idx[2];
                if (!needed && s->age[k] < best) { best = s->age[k]; slot = k; }
            }
            if (slot < 0) slot = 7;
        }
        f->refresh_flags = (uint8_t)(1u << slot);
        s->surface[slot] = current;
    }
    s->age[slot] = ++s->clock;
    return slot;
}

int vp9_context_score(const Vp9Probs *c, const Vp9Probs *to) {
    const uint8_t *a = c->mv_joint, *b = to->mv_joint;
    int n = (int)(sizeof *c - offsetof(Vp9Probs, mv_joint)), bad = 0, same = 0;
    for (int i = 0; i < n; i++) { if (a[i] == b[i]) same++; else if (!(b[i] & 1)) bad++; }
    return same - 1000 * bad;
}

/* ---- plain bits (the uncompressed header) ---------------------------------------- */
typedef struct Bits { uint8_t *out; uint32_t cap, pos; } Bits;   /* pos in bits */

static void put(Bits *b, uint32_t value, int n) {
    while (n--) {
        uint32_t byte = b->pos >> 3;
        if (byte >= b->cap) { b->pos++; continue; }
        if (!(b->pos & 7)) b->out[byte] = 0;
        if (value >> n & 1) b->out[byte] |= (uint8_t)(0x80 >> (b->pos & 7));
        b->pos++;
    }
}
static void put_signed(Bits *b, int value, int n) {   /* su(n): magnitude, then sign */
    put(b, (uint32_t)(value < 0 ? -value : value), n);
    put(b, value < 0, 1);
}

/* ---- the boolean coder (the compressed header) - libvpx's vpx_writer ----------------- */
typedef struct Bool { uint8_t *out; uint32_t cap, pos, low, range; int count; } Bool;

static void bool_start(Bool *w, uint8_t *out, uint32_t cap) {
    memset(w, 0, sizeof *w);
    w->out = out; w->cap = cap; w->range = 255; w->count = -24;
}
static void bool_put(Bool *w, int bit, int prob) {
    uint32_t split = 1 + (((w->range - 1) * (uint32_t)prob) >> 8);
    uint32_t range = bit ? w->range - split : split, low = w->low + (bit ? split : 0);
    int shift = 0;
    while ((range << shift) < 128) shift++;
    range <<= shift;
    int count = w->count + shift;
    if (count >= 0) {
        int offset = shift - count;
        if ((low << (offset - 1)) & 0x80000000u) {   /* carry into what is written */
            int x = (int)w->pos - 1;
            while (x >= 0 && w->out[x] == 0xFF) { w->out[x] = 0; x--; }
            if (x >= 0) w->out[x]++;
        }
        if (w->pos < w->cap) w->out[w->pos] = (uint8_t)(low >> (24 - offset));
        w->pos++;
        low <<= offset;
        shift = count;
        low &= 0xFFFFFF;
        count -= 8;
    }
    low <<= shift;
    w->count = count;
    w->low = low;
    w->range = range;
}
static void bool_literal(Bool *w, uint32_t value, int n) { while (n--) bool_put(w, value >> n & 1, 128); }
static uint32_t bool_end(Bool *w) {
    for (int i = 0; i < 32; i++) bool_put(w, 0, 128);
    if (w->pos && w->pos < w->cap && (w->out[w->pos - 1] & 0xE0) == 0xC0) w->out[w->pos++] = 0;   /* not mistakable for a superframe index */
    return w->pos;
}

/* ---- probability updates (6.3.5, 6.3.6) ------------------------------------------ */
static int inv_recenter_nonneg(int v, int m) {
    if (v > 2 * m) return v;
    return v & 1 ? m - ((v + 1) >> 1) : m + (v >> 1);
}

static const uint8_t *inv_map(void) {
    static uint8_t table[255];
    if (!table[0]) {
        int n = 0;
        for (int k = 0; k < 20; k++) table[n++] = (uint8_t)(7 + 13 * k);
        for (int v = 1; v <= 253; v++)
            if ((v - 7) % 13 != 0 || v < 7) table[n++] = (uint8_t)v;
        table[254] = 253;
    }
    return table;
}

int vp9_inv_remap_prob(int delta, int old_prob) {
    int v = inv_map()[delta], m = old_prob - 1;
    if ((m << 1) <= 255) return 1 + inv_recenter_nonneg(v, m);
    return 255 - inv_recenter_nonneg(v, 255 - 1 - m);
}

int vp9_remap_prob(int new_prob, int old_prob) {
    static uint8_t table[256][256];   /* [old][new]: delta + 1 (0: not computed) */
    if (!table[old_prob][new_prob])
        for (int d = 0; d < 254; d++) {
            int p = vp9_inv_remap_prob(d, old_prob);
            if (p >= 1 && p <= 255 && !table[old_prob][p]) table[old_prob][p] = (uint8_t)(d + 1);
        }
    return table[old_prob][new_prob] - 1;
}

static void put_subexp(Bool *w, int d) {   /* inverse of decode_term_subexp */
    if (d < 16) { bool_literal(w, 0, 1); bool_literal(w, (uint32_t)d, 4); return; }
    bool_literal(w, 1, 1);
    if (d < 32) { bool_literal(w, 0, 1); bool_literal(w, (uint32_t)(d - 16), 4); return; }
    bool_literal(w, 1, 1);
    if (d < 64) { bool_literal(w, 0, 1); bool_literal(w, (uint32_t)(d - 32), 5); return; }
    bool_literal(w, 1, 1);
    if (d < 129) { bool_literal(w, (uint32_t)(d - 64), 7); return; }
    bool_literal(w, (uint32_t)((d + 1) >> 1), 7);
    bool_literal(w, (uint32_t)((d + 1) & 1), 1);
}

/* An update pass: probabilities are named by where they are in `from`; the same offset in `to`
 * is the target, and in `res` what the decoder ends up with. */
typedef struct Upd { Bool w; const uint8_t *from, *to; uint8_t *res; } Upd;

#define UPDATE_PROB 252
static void diff_updates(Upd *u, const uint8_t *field, int n) {
    size_t at = (size_t)(field - u->from);
    for (int i = 0; i < n; i++) {
        uint8_t from = u->from[at + i], to = u->to[at + i];
        int d = to != from ? vp9_remap_prob(to, from) : -1;
        bool_put(&u->w, d >= 0, UPDATE_PROB);
        if (d >= 0) put_subexp(&u->w, d);
        u->res[at + i] = d >= 0 ? (uint8_t)vp9_inv_remap_prob(d, from) : from;
    }
}
static void mv_updates(Upd *u, const uint8_t *field, int n) {   /* motion vector probabilities: 7 bits, so always odd */
    size_t at = (size_t)(field - u->from);
    for (int i = 0; i < n; i++) {
        uint8_t from = u->from[at + i], to = u->to[at + i];
        int update = to != from && (to | 1) != from;
        bool_put(&u->w, update, UPDATE_PROB);
        if (update) bool_literal(&u->w, to >> 1, 7);
        u->res[at + i] = update ? (uint8_t)(to | 1) : from;
    }
}

/* ---- the uncompressed header (6.2) ---------------------------------------------- */
static void color_config(Bits *b, const Vp9Frame *f) {
    if (f->profile >= 2) put(b, f->bit_depth == 12, 1);
    put(b, f->color_space, 3);
    if (f->color_space != 7) {   /* not sRGB */
        put(b, f->color_range, 1);
        if (f->profile == 1 || f->profile == 3) { put(b, f->subsampling_x, 1); put(b, f->subsampling_y, 1); put(b, 0, 1); }
    } else if (f->profile == 1 || f->profile == 3)
        put(b, 0, 1);
}

static void frame_size(Bits *b, const Vp9Frame *f) {
    put(b, f->width - 1u, 16);
    put(b, f->height - 1u, 16);
}

static void render_size(Bits *b, const Vp9Frame *f) {
    int differ = f->render_width && (f->render_width != f->width || f->render_height != f->height);
    put(b, differ, 1);
    if (differ) { put(b, f->render_width - 1u, 16); put(b, f->render_height - 1u, 16); }
}

static void tile_log2_bounds(const Vp9Frame *f, int *min_log2, int *max_log2) {
    int sb64_cols = ((f->width + 7) / 8 + 7) / 8, lo = 0, hi = 1;
    while ((64 << lo) < sb64_cols) lo++;
    while ((sb64_cols >> hi) >= 4) hi++;
    *min_log2 = lo;
    *max_log2 = hi - 1;
}

uint32_t vp9_write_uncompressed(const Vp9Frame *f, uint32_t compressed_size, uint8_t *out, uint32_t cap) {
    Bits b = {out, cap, 0};
    put(&b, 2, 2);   /* frame marker */
    put(&b, f->profile & 1, 1);
    put(&b, f->profile >> 1, 1);
    if (f->profile == 3) put(&b, 0, 1);
    put(&b, f->show_existing, 1);
    if (f->show_existing) {
        put(&b, f->existing_idx, 3);
        return (b.pos + 7) >> 3;
    }
    put(&b, !f->key, 1);   /* frame_type: 0 is a key frame */
    put(&b, f->show, 1);
    put(&b, f->error_resilient, 1);
    int intra = f->key || f->intra_only;
    if (f->key) {
        put(&b, 0x498342, 24);   /* sync code */
        color_config(&b, f);
        frame_size(&b, f);
        render_size(&b, f);
    } else {
        if (!f->show) put(&b, f->intra_only, 1);
        if (!f->error_resilient) put(&b, f->reset_context, 2);
        if (f->intra_only) {
            put(&b, 0x498342, 24);
            if (f->profile > 0) color_config(&b, f);
            put(&b, f->refresh_flags, 8);
            frame_size(&b, f);
            render_size(&b, f);
        } else {
            put(&b, f->refresh_flags, 8);
            for (int i = 0; i < 3; i++) { put(&b, f->ref_idx[i], 3); put(&b, f->sign_bias[i], 1); }
            for (int i = 0; i < 3; i++) {   /* frame_size_with_refs */
                int found = f->found_ref == i + 1;
                put(&b, found, 1);
                if (found) break;
            }
            if (!f->found_ref) frame_size(&b, f);
            render_size(&b, f);
            put(&b, f->allow_hp, 1);
            put(&b, f->interp_filter == VP9_SWITCHABLE, 1);
            if (f->interp_filter != VP9_SWITCHABLE) {
                static const uint8_t literal_of[4] = {1, 0, 2, 3};   /* eighttap, smooth, sharp, bilinear */
                put(&b, literal_of[f->interp_filter], 2);
            }
        }
    }
    if (!f->error_resilient) { put(&b, f->refresh_context, 1); put(&b, f->parallel, 1); }
    put(&b, f->context_idx, 2);
    /* loop filter (6.2.8) */
    put(&b, f->lf_level, 6);
    put(&b, f->lf_sharpness, 3);
    put(&b, f->lf_delta_enabled, 1);
    if (f->lf_delta_enabled) {
        put(&b, f->lf_delta_update, 1);
        if (f->lf_delta_update) {
            for (int i = 0; i < 4; i++) { put(&b, f->lf_ref_update[i], 1); if (f->lf_ref_update[i]) put_signed(&b, f->lf_ref[i], 6); }
            for (int i = 0; i < 2; i++) { put(&b, f->lf_mode_update[i], 1); if (f->lf_mode_update[i]) put_signed(&b, f->lf_mode[i], 6); }
        }
    }
    /* quantization (6.2.9) */
    put(&b, f->base_q, 8);
    const int8_t dq[3] = {f->dq_y_dc, f->dq_uv_dc, f->dq_uv_ac};
    for (int i = 0; i < 3; i++) { put(&b, dq[i] != 0, 1); if (dq[i]) put_signed(&b, dq[i], 4); }
    /* segmentation (6.2.11) */
    put(&b, f->seg_enabled, 1);
    if (f->seg_enabled) {
        put(&b, f->seg_update_map, 1);
        if (f->seg_update_map) {
            for (int i = 0; i < 7; i++) { put(&b, f->seg_tree[i] != 255, 1); if (f->seg_tree[i] != 255) put(&b, f->seg_tree[i], 8); }
            put(&b, f->seg_temporal, 1);
            if (f->seg_temporal)
                for (int i = 0; i < 3; i++) { put(&b, f->seg_pred[i] != 255, 1); if (f->seg_pred[i] != 255) put(&b, f->seg_pred[i], 8); }
        }
        put(&b, f->seg_update_data, 1);
        if (f->seg_update_data) {
            static const uint8_t bits[4] = {8, 6, 2, 0}, is_signed[4] = {1, 1, 0, 0};
            put(&b, f->seg_abs, 1);
            for (int i = 0; i < 8; i++)
                for (int j = 0; j < 4; j++) {
                    put(&b, f->seg_on[i][j], 1);
                    if (!f->seg_on[i][j]) continue;
                    int v = f->seg_data[i][j];
                    put(&b, (uint32_t)(v < 0 ? -v : v), bits[j]);
                    if (is_signed[j]) put(&b, v < 0, 1);
                }
        }
    }
    /* tiles (6.2.12) */
    int min_log2, max_log2;
    tile_log2_bounds(f, &min_log2, &max_log2);
    for (int n = min_log2; n < max_log2; n++) {
        put(&b, n < f->tile_cols_log2, 1);
        if (n >= f->tile_cols_log2) break;
    }
    put(&b, f->tile_rows_log2 != 0, 1);
    if (f->tile_rows_log2) put(&b, f->tile_rows_log2 != 1, 1);
    put(&b, compressed_size, 16);
    (void)intra;
    return (b.pos + 7) >> 3;
}

/* ---- the compressed header (6.3) ------------------------------------------------ */
uint32_t vp9_write_compressed(const Vp9Frame *f, const Vp9Probs *from, const Vp9Probs *to, Vp9Probs *result, uint8_t *out, uint32_t cap) {
    Vp9Probs scratch;
    if (!result) result = &scratch;
    *result = *from;
    Upd u = {{0}, (const uint8_t *)from, (const uint8_t *)to, (uint8_t *)result};
    Bool *w = &u.w;
    bool_start(w, out, cap);
    bool_put(w, 0, 128);   /* the marker bit */
    int lossless = f->base_q == 0 && !f->dq_y_dc && !f->dq_uv_dc && !f->dq_uv_ac;
    if (!lossless) {   /* tx_mode */
        bool_literal(w, f->tx_mode < VP9_ALLOW_32X32 ? f->tx_mode : VP9_ALLOW_32X32, 2);
        if (f->tx_mode >= VP9_ALLOW_32X32) bool_literal(w, f->tx_mode == VP9_TX_MODE_SELECT, 1);
    }
    int tx_mode = lossless ? VP9_ONLY_4X4 : f->tx_mode;
    if (tx_mode == VP9_TX_MODE_SELECT) {
        diff_updates(&u, &from->tx8[0][0], 2);
        diff_updates(&u, &from->tx16[0][0], 4);
        diff_updates(&u, &from->tx32[0][0], 6);
    }
    /* coefficients, per transform size up to the largest allowed */
    static const uint8_t biggest[5] = {0, 1, 2, 3, 3};
    for (int t = 0; t <= biggest[tx_mode]; t++) {
        int any = 0;
        for (int i = 0; i < 2 * 2 * 6 * 6 * 3 && !any; i++) {
            int band = i / (6 * 3) % 6, ctx = i / 3 % 6;
            if (band == 0 && ctx >= 3) continue;
            any = (&from->coef[t][0][0][0][0][0])[i] != (&to->coef[t][0][0][0][0][0])[i];
        }
        bool_literal(w, any, 1);
        if (!any) continue;
        for (int i = 0; i < 2; i++)
            for (int j = 0; j < 2; j++)
                for (int k = 0; k < 6; k++)
                    for (int l = 0; l < (k == 0 ? 3 : 6); l++)
                        diff_updates(&u, from->coef[t][i][j][k][l], 3);
    }
    diff_updates(&u, from->skip, 3);
    if (!f->key && !f->intra_only) {
        diff_updates(&u, &from->inter_mode[0][0], 7 * 3);
        if (f->interp_filter == VP9_SWITCHABLE) diff_updates(&u, &from->interp_filter[0][0], 4 * 2);
        diff_updates(&u, from->is_inter, 4);
        /* reference mode (6.3.11): compound prediction is possible when the references point both ways */
        if (f->sign_bias[1] != f->sign_bias[0] || f->sign_bias[2] != f->sign_bias[0]) {
            bool_literal(w, f->reference_mode != VP9_SINGLE_REFERENCE, 1);
            if (f->reference_mode != VP9_SINGLE_REFERENCE) bool_literal(w, f->reference_mode == VP9_REFERENCE_MODE_SELECT, 1);
        }
        if (f->reference_mode == VP9_REFERENCE_MODE_SELECT) diff_updates(&u, from->comp_mode, 5);
        if (f->reference_mode != VP9_COMPOUND_REFERENCE) diff_updates(&u, &from->single_ref[0][0], 10);
        if (f->reference_mode != VP9_SINGLE_REFERENCE) diff_updates(&u, from->comp_ref, 5);
        diff_updates(&u, &from->y_mode[0][0], 4 * 9);
        diff_updates(&u, &from->partition[0][0], 16 * 3);
        /* motion vectors (6.3.17) */
        mv_updates(&u, from->mv_joint, 3);
        for (int i = 0; i < 2; i++) {
            mv_updates(&u, &from->mv_sign[i], 1);
            mv_updates(&u, from->mv_class[i], 10);
            mv_updates(&u, &from->mv_class0_bit[i], 1);
            mv_updates(&u, from->mv_bits[i], 10);
        }
        for (int i = 0; i < 2; i++) {
            mv_updates(&u, &from->mv_class0_fr[i][0][0], 6);
            mv_updates(&u, from->mv_fr[i], 3);
        }
        if (f->allow_hp)
            for (int i = 0; i < 2; i++) {
                mv_updates(&u, &from->mv_class0_hp[i], 1);
                mv_updates(&u, &from->mv_hp[i], 1);
            }
    }
    return bool_end(w);
}
