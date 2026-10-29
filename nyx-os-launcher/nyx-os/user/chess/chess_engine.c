// user/chess/chess_engine.c — the whole engine: board, movegen, legality,
// evaluation, iterative-deepening negamax with alpha-beta + quiescence.
//
// Freestanding: only <stdint.h>.  Runs as a ring-3 PE task on Nyx.
//
// Board layout: int8_t board[64], a1 = 0 .. h1 = 7, a8 = 56 .. h8 = 63.
// Sign encodes side (white positive, black negative), magnitude the
// piece type 1..6 = P N B R Q K.  White pawns move +8.
//
// Move pipeline: pseudo-legal generator → make/unmake on the real
// position → "own king attacked?" filter → legal list.  Evaluation is
// material + piece-square tables (+bishop pair, tempo, endgame king
// table).  Search is iterative-deepening negamax with alpha-beta,
// MVV-LVA capture ordering, check extension and a capture-only
// quiescence search.  A hard node budget bounds every call so the UI
// never locks up longer than requested.
//
// Repetition inside the search tree is scored as a draw via a rolling
// FNV signature stack (no transposition table — this engine stays
// deliberately small and debuggable).

#include "chess.h"

// ─────────────────────────────────────────────────────────────────
// Basics
// ─────────────────────────────────────────────────────────────────
#define MATE      30000
#define MATE_BOND 256        // |score| >= MATE-BOND ⇒ forced mate score
#define INF       (MATE + 1)

static int VAL[7] = { 0, 100, 320, 330, 500, 900, 0 };   // P N B R Q K

#define RNK(sq) ((sq) >> 3)
#define FIL(sq) ((sq) & 7)

typedef struct {
    int8_t  capt;       // signed piece captured (0 none)
    uint8_t castle;     // old castle rights
    int8_t  ep;         // old ep square
    int16_t halfmove;   // old 50-move clock
    int8_t  wk, bk;     // old king squares
} undo_t;

// ─────────────────────────────────────────────────────────────────
// Precomputed tables (lazily built once)
// ─────────────────────────────────────────────────────────────────
static uint8_t knight_tbl[64][9];   // target list, 0xFF-terminated (a1
static uint8_t king_tbl[64][9];     // is square 0 — 0 can't terminate!)
static int     tables_ready = 0;

static void init_tables(void) {
    if (tables_ready) return;
    static const int kdr[8] = { -2,-2,-1,-1, 1, 1, 2, 2 };
    static const int kdf[8] = { -1, 1,-2, 2,-2, 2,-1, 1 };
    static const int gdr[8] = { -1,-1,-1, 0, 0, 1, 1, 1 };
    static const int gdf[8] = { -1, 0, 1,-1, 1,-1, 0, 1 };
    for (int s = 0; s < 64; s++) {
        int n = 0, k = 0;
        for (int i = 0; i < 8; i++) {
            int r = RNK(s) + kdr[i], f = FIL(s) + kdf[i];
            if (r >= 0 && r < 8 && f >= 0 && f < 8) knight_tbl[s][n++] = (uint8_t)(r*8+f);
        }
        knight_tbl[s][n] = 0xFF;
        for (int i = 0; i < 8; i++) {
            int r = RNK(s) + gdr[i], f = FIL(s) + gdf[i];
            if (r >= 0 && r < 8 && f >= 0 && f < 8) king_tbl[s][k++] = (uint8_t)(r*8+f);
        }
        king_tbl[s][k] = 0xFF;
    }
    tables_ready = 1;
}

// rights &= mask[from] & mask[to] — bit0 W-O-O, bit1 W-O-O-O,
// bit2 B-O-O, bit3 B-O-O-O.
static uint8_t castle_mask[64];

static void init_masks(void) {
    for (int i = 0; i < 64; i++) castle_mask[i] = 15;
    castle_mask[4]  = 15 & ~(1 | 2);   // e1
    castle_mask[0]  = 15 & ~2;         // a1
    castle_mask[7]  = 15 & ~1;         // h1
    castle_mask[60] = 15 & ~(4 | 8);   // e8
    castle_mask[56] = 15 & ~8;         // a8
    castle_mask[63] = 15 & ~4;         // h8
}

// ─────────────────────────────────────────────────────────────────
// Attack detection: is `sq` attacked by side `by` (+1/-1)?
// ─────────────────────────────────────────────────────────────────
static const int rook_dr[4]   = { 1, -1, 0, 0 };
static const int rook_df[4]   = { 0, 0, 1, -1 };
static const int bish_dr[4]   = { 1, 1, -1, -1 };
static const int bish_df[4]   = { 1, -1, 1, -1 };

static int attacked(const chess_pos_t* p, int sq, int by) {
    const int8_t* b = p->board;

    // Pawns.  A white pawn on s attacks s+7 (its file-1) and s+9
    // (its file+1); invert for the reverse lookup:
    //   sq attacked via  s = sq-7  ⇔  pawn file = file(sq)+1 (no wrap)
    //   sq attacked via  s = sq-9  ⇔  pawn file = file(sq)-1
    if (by > 0) {
        if (FIL(sq) < 7 && sq >= 7  && b[sq - 7] == P_WP) return 1;
        if (FIL(sq) > 0 && sq >= 9  && b[sq - 9] == P_WP) return 1;
    } else {
        if (FIL(sq) > 0 && sq + 7 < 64 && b[sq + 7] == -PAWN) return 1;
        if (FIL(sq) < 7 && sq + 9 < 64 && b[sq + 9] == -PAWN) return 1;
    }

    // Knights and king (precomputed, no wrap hazards).
    for (int i = 0; knight_tbl[sq][i] != 0xFF; i++)
        if (b[knight_tbl[sq][i]] == by * KNIGHT) return 1;
    for (int i = 0; king_tbl[sq][i] != 0xFF; i++)
        if (b[king_tbl[sq][i]] == by * KING) return 1;

    // Sliders.
    for (int d = 0; d < 4; d++) {
        int r = RNK(sq) + rook_dr[d], f = FIL(sq) + rook_df[d];
        while (r >= 0 && r < 8 && f >= 0 && f < 8) {
            int8_t pc = b[r*8+f];
            if (pc) {
                if (pc == by * ROOK || pc == by * QUEEN) return 1;
                break;
            }
            r += rook_dr[d]; f += rook_df[d];
        }
    }
    for (int d = 0; d < 4; d++) {
        int r = RNK(sq) + bish_dr[d], f = FIL(sq) + bish_df[d];
        while (r >= 0 && r < 8 && f >= 0 && f < 8) {
            int8_t pc = b[r*8+f];
            if (pc) {
                if (pc == by * BISHOP || pc == by * QUEEN) return 1;
                break;
            }
            r += bish_dr[d]; f += bish_df[d];
        }
    }
    return 0;
}

int chess_in_check(const chess_pos_t* p) {
    int k = p->side > 0 ? p->ksq[0] : p->ksq[1];
    return attacked(p, k, -p->side);
}

// ─────────────────────────────────────────────────────────────────
// Make / unmake
// ─────────────────────────────────────────────────────────────────
static void make(chess_pos_t* p, const chess_move_t* m, undo_t* u) {
    int side = p->side;
    int8_t pc = p->board[m->from];

    u->capt    = p->board[m->to];
    u->castle  = p->castle;
    u->ep      = p->ep;
    u->halfmove= p->halfmove;
    u->wk      = p->ksq[0];
    u->bk      = p->ksq[1];

    // En-passant capture removes the bypassed pawn.
    if (m->flags & MV_EP) {
        int cap = m->to - 8 * side;
        u->capt   = p->board[cap];
        p->board[cap] = 0;
    }

    p->board[m->to]   = m->promo ? (int8_t)(m->promo * side) : pc;
    p->board[m->from] = 0;

    if (m->flags & MV_CASTLE) {
        // King already moved above; now the rook.
        if (m->to == m->from + 2) {                    // O-O
            p->board[m->from + 1] = p->board[m->from + 3];
            p->board[m->from + 3] = 0;
        } else {                                       // O-O-O
            p->board[m->from - 1] = p->board[m->from - 4];
            p->board[m->from - 4] = 0;
        }
    }

    if (pc == KING * side) p->ksq[side > 0 ? 0 : 1] = (int8_t)m->to;

    p->castle &= (uint8_t)(castle_mask[m->from] & castle_mask[m->to]);
    p->ep      = (m->flags & MV_DPP) ? (int8_t)(m->from + 8 * side) : -1;
    p->halfmove = (int16_t)(((pc == PAWN * side) || u->capt) ? 0 : p->halfmove + 1);
    p->side    = (int8_t)-side;
    p->ply++;
}

static void unmake(chess_pos_t* p, const chess_move_t* m, const undo_t* u) {
    p->side    = (int8_t)-p->side;
    int side   = p->side;
    int8_t pc  = p->board[m->to];

    p->board[m->from] = m->promo ? (int8_t)(PAWN * side) : pc;
    p->board[m->to]   = u->capt;

    if (m->flags & MV_EP) {
        // make() saved the bypassed pawn in u->capt and left board[to]
        // empty — restore it on its own square, nothing on `to`.
        p->board[m->to] = 0;
        p->board[m->to - 8 * side] = u->capt;
    }

    if (m->flags & MV_CASTLE) {
        if (m->to == m->from + 2) {
            p->board[m->from + 3] = p->board[m->from + 1];
            p->board[m->from + 1] = 0;
        } else {
            p->board[m->from - 4] = p->board[m->from - 1];
            p->board[m->from - 1] = 0;
        }
    }

    if (pc == KING * side) p->ksq[side > 0 ? 0 : 1] = (int8_t)m->from;

    p->castle   = u->castle;
    p->ep       = u->ep;
    p->halfmove = u->halfmove;
    p->ksq[0]   = u->wk;
    p->ksq[1]   = u->bk;
    p->ply--;
}

// ─────────────────────────────────────────────────────────────────
// Pseudo-legal generation (all moves; `caps_only` = captures+promos)
// ─────────────────────────────────────────────────────────────────
static int gen_pseudo(const chess_pos_t* p, chess_move_t* out, int caps_only) {
    int n = 0, side = p->side;
    const int8_t* b = p->board;

    for (int s = 0; s < 64; s++) {
        int8_t pc = b[s];
        if (!pc || (pc > 0) != (side > 0)) continue;
        int t = pc > 0 ? pc : -pc;

        if (t == PAWN) {
            int fwd = 8 * side;
            int prom_rank = side > 0 ? 7 : 0;
            int one = s + fwd;
            if (one >= 0 && one < 64 && !b[one]) {
                if (RNK(one) == prom_rank) {
                    for (int pr = QUEEN; pr >= KNIGHT; pr--) {
                        out[n].from = (uint8_t)s; out[n].to = (uint8_t)one;
                        out[n].promo = (uint8_t)pr; out[n].flags = 0; n++;
                    }
                } else if (!caps_only) {
                    out[n].from = (uint8_t)s; out[n].to = (uint8_t)one;
                    out[n].promo = 0; out[n].flags = 0; n++;
                    int start_rank = side > 0 ? 1 : 6;
                    int two = s + 2 * fwd;
                    if (RNK(s) == start_rank && !b[two]) {
                        out[n].from = (uint8_t)s; out[n].to = (uint8_t)two;
                        out[n].promo = 0; out[n].flags = MV_DPP; n++;
                    }
                }
            }
            // Pawn captures (incl. promotions-with-capture, ep).
            static const int pdf[2] = { -1, 1 };
            for (int i = 0; i < 2; i++) {
                int r = RNK(s) + side, f = FIL(s) + pdf[i];
                if (r < 0 || r > 7 || f < 0 || f > 7) continue;
                int d = r * 8 + f;
                int is_ep  = (d == p->ep);
                int is_cap = b[d] && (b[d] > 0) != (side > 0);
                if (!is_cap && !is_ep) continue;
                if (RNK(d) == prom_rank) {
                    for (int pr = QUEEN; pr >= KNIGHT; pr--) {
                        out[n].from = (uint8_t)s; out[n].to = (uint8_t)d;
                        out[n].promo = (uint8_t)pr;
                        out[n].flags = (uint8_t)(is_ep ? MV_EP : MV_CAP); n++;
                    }
                } else {
                    out[n].from = (uint8_t)s; out[n].to = (uint8_t)d;
                    out[n].promo = 0;
                    out[n].flags = (uint8_t)(is_ep ? MV_EP : MV_CAP); n++;
                }
            }
        } else if (t == KNIGHT || t == KING) {
            const uint8_t* tbl = (t == KNIGHT) ? knight_tbl[s] : king_tbl[s];
            for (int i = 0; tbl[i] != 0xFF; i++) {
                int d = tbl[i];
                int8_t q = b[d];
                if (q && (q > 0) == (side > 0)) continue;        // own piece
                if (!q && caps_only) continue;                   // quiet
                out[n].from = (uint8_t)s; out[n].to = (uint8_t)d;
                out[n].promo = 0;
                out[n].flags = q ? MV_CAP : 0; n++;
            }
            // Castling (king only).
            if (t == KING && !caps_only) {
                if (side > 0 && s == 4) {
                    if ((p->castle & 1) && !b[5] && !b[6] && b[7] == P_WR &&
                        !attacked(p, 4, -1) && !attacked(p, 5, -1) &&
                        !attacked(p, 6, -1)) {
                        out[n].from = 4; out[n].to = 6; out[n].promo = 0;
                        out[n].flags = MV_CASTLE; n++;
                    }
                    if ((p->castle & 2) && !b[3] && !b[2] && !b[1] &&
                        b[0] == P_WR &&
                        !attacked(p, 4, -1) && !attacked(p, 3, -1) &&
                        !attacked(p, 2, -1)) {
                        out[n].from = 4; out[n].to = 2; out[n].promo = 0;
                        out[n].flags = MV_CASTLE; n++;
                    }
                } else if (side < 0 && s == 60) {
                    if ((p->castle & 4) && !b[61] && !b[62] && b[63] == -ROOK &&
                        !attacked(p, 60, 1) && !attacked(p, 61, 1) &&
                        !attacked(p, 62, 1)) {
                        out[n].from = 60; out[n].to = 62; out[n].promo = 0;
                        out[n].flags = MV_CASTLE; n++;
                    }
                    if ((p->castle & 8) && !b[59] && !b[58] && !b[57] &&
                        b[56] == -ROOK &&
                        !attacked(p, 60, 1) && !attacked(p, 59, 1) &&
                        !attacked(p, 58, 1)) {
                        out[n].from = 60; out[n].to = 58; out[n].promo = 0;
                        out[n].flags = MV_CASTLE; n++;
                    }
                }
            }
        } else {
            // Sliders: rook dirs then bishop dirs (queen = both).
            for (int half = 0; half < 2; half++) {
                if (t == ROOK && half == 1) break;
                if (t == BISHOP && half == 0) continue;
                const int* dr = half ? bish_dr : rook_dr;
                const int* df = half ? bish_df : rook_df;
                for (int d = 0; d < 4; d++) {
                    int r = RNK(s) + dr[d], f = FIL(s) + df[d];
                    while (r >= 0 && r < 8 && f >= 0 && f < 8) {
                        int q = r*8+f, pcq = b[q];
                        if (pcq && (pcq > 0) == (side > 0)) break;
                        if (!pcq && caps_only) break;
                        out[n].from = (uint8_t)s; out[n].to = (uint8_t)q;
                        out[n].promo = 0;
                        out[n].flags = pcq ? MV_CAP : 0; n++;
                        if (pcq) break;
                        r += dr[d]; f += df[d];
                    }
                }
            }
        }
    }
    return n;
}

// Generate LEGAL moves (filters pseudo-legal through make/unmake).
int chess_gen(const chess_pos_t* p, chess_move_t* out) {
    init_tables();
    init_masks();
    chess_pos_t tmp = *p;
    int n = gen_pseudo(&tmp, out, 0);
    int k = 0;
    for (int i = 0; i < n; i++) {
        undo_t u;
        make(&tmp, &out[i], &u);
        int king = tmp.side > 0 ? tmp.ksq[1] : tmp.ksq[0];   // moved side
        if (!attacked(&tmp, king, tmp.side)) out[k++] = out[i];
        unmake(&tmp, &out[i], &u);
    }
    return k;
}

// Captures/promotions only (for quiescence), legal-filtered.
static int gen_caps(const chess_pos_t* p, chess_move_t* out) {
    chess_pos_t tmp = *p;
    int n = gen_pseudo(&tmp, out, 1);
    int k = 0;
    for (int i = 0; i < n; i++) {
        undo_t u;
        make(&tmp, &out[i], &u);
        int king = tmp.side > 0 ? tmp.ksq[1] : tmp.ksq[0];
        if (!attacked(&tmp, king, tmp.side)) out[k++] = out[i];
        unmake(&tmp, &out[i], &u);
    }
    return k;
}

// ─────────────────────────────────────────────────────────────────
// Evaluation — from WHITE's perspective, then side-relative at call.
// ─────────────────────────────────────────────────────────────────
// Tables are written rank-8-first (the usual printed form); white uses
// the mirrored index, black the direct one.
static const int pst_pawn[64] = {
      0,  0,  0,  0,  0,  0,  0,  0,
     50, 50, 50, 50, 50, 50, 50, 50,
     10, 10, 20, 30, 30, 20, 10, 10,
      5,  5, 10, 25, 25, 10,  5,  5,
      0,  0,  0, 20, 20,  0,  0,  0,
      5, -5,-10,  0,  0,-10, -5,  5,
      5, 10, 10,-20,-20, 10, 10,  5,
      0,  0,  0,  0,  0,  0,  0,  0 };
static const int pst_knight[64] = {
    -50,-40,-30,-30,-30,-30,-40,-50,
    -40,-20,  0,  0,  0,  0,-20,-40,
    -30,  0, 10, 15, 15, 10,  0,-30,
    -30,  5, 15, 20, 20, 15,  5,-30,
    -30,  0, 15, 20, 20, 15,  0,-30,
    -30,  5, 10, 15, 15, 10,  5,-30,
    -40,-20,  0,  5,  5,  0,-20,-40,
    -50,-40,-30,-30,-30,-30,-40,-50 };
static const int pst_bishop[64] = {
    -20,-10,-10,-10,-10,-10,-10,-20,
    -10,  0,  0,  0,  0,  0,  0,-10,
    -10,  0,  5, 10, 10,  5,  0,-10,
    -10,  5,  5, 10, 10,  5,  5,-10,
    -10,  0, 10, 10, 10, 10,  0,-10,
    -10, 10, 10, 10, 10, 10, 10,-10,
    -10,  5,  0,  0,  0,  0,  5,-10,
    -20,-10,-10,-10,-10,-10,-10,-20 };
static const int pst_rook[64] = {
      0,  0,  0,  0,  0,  0,  0,  0,
      5, 10, 10, 10, 10, 10, 10,  5,
     -5,  0,  0,  0,  0,  0,  0, -5,
     -5,  0,  0,  0,  0,  0,  0, -5,
     -5,  0,  0,  0,  0,  0,  0, -5,
     -5,  0,  0,  0,  0,  0,  0, -5,
     -5,  0,  0,  0,  0,  0,  0, -5,
      0,  0,  0,  5,  5,  0,  0,  0 };
static const int pst_queen[64] = {
    -20,-10,-10, -5, -5,-10,-10,-20,
    -10,  0,  0,  0,  0,  0,  0,-10,
    -10,  0,  5,  5,  5,  5,  0,-10,
     -5,  0,  5,  5,  5,  5,  0, -5,
      0,  0,  5,  5,  5,  5,  0, -5,
    -10,  5,  5,  5,  5,  5,  0,-10,
    -10,  0,  5,  0,  0,  0,  0,-10,
    -20,-10,-10, -5, -5,-10,-10,-20 };
static const int pst_king_mg[64] = {
    -30,-40,-40,-50,-50,-40,-40,-30,
    -30,-40,-40,-50,-50,-40,-40,-30,
    -30,-40,-40,-50,-50,-40,-40,-30,
    -30,-40,-40,-50,-50,-40,-40,-30,
    -20,-30,-30,-40,-40,-30,-30,-20,
    -10,-20,-20,-20,-20,-20,-20,-10,
     20, 20,  0,  0,  0,  0, 20, 20,
     20, 30, 10,  0,  0, 10, 30, 20 };
static const int pst_king_eg[64] = {
    -50,-40,-30,-20,-20,-30,-40,-50,
    -30,-20,-10,  0,  0,-10,-20,-30,
    -30,-10, 20, 30, 30, 20,-10,-30,
    -30,-10, 30, 40, 40, 30,-10,-30,
    -30,-10, 30, 40, 40, 30,-10,-30,
    -30,-10, 20, 30, 30, 20,-10,-30,
    -30,-30,  0,  0,  0,  0,-30,-30,
    -50,-30,-30,-30,-30,-30,-30,-50 };

static const int* PST[7] = { 0, pst_pawn, pst_knight, pst_bishop,
                             pst_rook, pst_queen, pst_king_mg };

static int evaluate(const chess_pos_t* p) {
    int score = 0, npm = 0, wb_cnt = 0, bb_cnt = 0;
    const int8_t* b = p->board;

    for (int s = 0; s < 64; s++) {
        int8_t pc = b[s];
        if (!pc) continue;
        int t = pc > 0 ? pc : -pc;
        int side = pc > 0 ? 1 : -1;
        int idx  = side > 0 ? (7 - RNK(s)) * 8 + FIL(s) : s;

        if (t != KING) {
            score += side * (VAL[t] + PST[t][idx]);
            if (t != PAWN) npm += VAL[t];
        }
        if (t == BISHOP) {
            if (side > 0) wb_cnt++;
            else          bb_cnt++;
        }
    }

    // Endgame king table when heavy pieces are gone.
    const int* kt = (npm < 2600) ? pst_king_eg : pst_king_mg;
    score += kt[(7 - RNK(p->ksq[0])) * 8 + FIL(p->ksq[0])];
    score -= kt[RNK(p->ksq[1]) * 8 + FIL(p->ksq[1])];

    // Bishop-pair bonus: two bishops cover both square colors.
    if (wb_cnt >= 2) score += 30;
    if (bb_cnt >= 2) score -= 30;

    score += (p->side > 0) ? 10 : -10;      // tempo
    return score;
}

// ─────────────────────────────────────────────────────────────────
// Search state
// ─────────────────────────────────────────────────────────────────
static uint64_t g_nodes;
static uint32_t g_cap;
static int      g_aborted;
static uint64_t g_sigs[128];     // repetition path (hash of position)
static int      g_sig_n;

static uint64_t fnv(const uint8_t* d, int n, uint64_t h) {
    for (int i = 0; i < n; i++) { h ^= d[i]; h *= 0x100000001b3ULL; }
    return h;
}
static uint64_t pos_sig(const chess_pos_t* p) {
    uint64_t h = 0xcbf29ce484222325ULL;
    h = fnv((const uint8_t*)p->board, 64, h);
    h = fnv((const uint8_t*)&p->side,   1, h);
    h = fnv((const uint8_t*)&p->castle, 1, h);
    h = fnv((const uint8_t*)&p->ep,     1, h);
    return h;
}

static void order_moves(chess_move_t* mv, int* sc, int n) {
    // Simple insertion sort, highest score first.
    for (int i = 1; i < n; i++) {
        chess_move_t m = mv[i]; int s = sc[i];
        int j = i - 1;
        while (j >= 0 && sc[j] < s) { mv[j+1] = mv[j]; sc[j+1] = sc[j]; j--; }
        mv[j+1] = m; sc[j+1] = s;
    }
}

static void score_moves(const chess_pos_t* p, const chess_move_t* mv,
                        int* sc, int n) {
    for (int i = 0; i < n; i++) {
        int s = 0;
        int8_t victim = mv[i].flags & MV_EP
            ? (int8_t)(PAWN * -p->side)
            : p->board[mv[i].to];
        if (victim) {
            int v = victim > 0 ? victim : -victim;
            int a = p->board[mv[i].from]; if (a < 0) a = -a;
            s = 100000 + VAL[v] * 16 - VAL[a];
        }
        if (mv[i].promo) s += 90000 + VAL[mv[i].promo];
        if ((mv[i].flags & MV_CASTLE)) s += 500;
        sc[i] = s;
    }
}

// Track best-move ordering between iterations at the root.
static void promote_best(chess_move_t* mv, int* sc, int n,
                         const chess_move_t* best) {
    for (int i = 1; i < n; i++)
        if (chess_move_eq(&mv[i], best)) {
            chess_move_t m = mv[i]; int s = sc[i];
            for (int j = i; j > 0; j--) { mv[j] = mv[j-1]; sc[j] = sc[j-1]; }
            mv[0] = m; sc[0] = s;
            return;
        }
}

static int quiesce(chess_pos_t* p, int alpha, int beta, int qply) {
    if (++g_nodes > g_cap) { g_aborted = 1; return alpha; }

    int stand = evaluate(p) * p->side;
    if (stand >= beta) return beta;
    if (stand > alpha) alpha = stand;
    if (qply >= 24) return alpha;

    chess_move_t mv[CHESS_MAXMOVES];
    int sc[CHESS_MAXMOVES];
    int n = gen_caps(p, mv);
    score_moves(p, mv, sc, n);
    order_moves(mv, sc, n);

    for (int i = 0; i < n; i++) {
        // Delta pruning: hopeless captures can't raise alpha.
        int8_t v = p->board[mv[i].to];
        int val = v ? VAL[v > 0 ? v : -v] : VAL[PAWN];
        if (stand + val + 200 <= alpha && !mv[i].promo) continue;

        undo_t u;
        make(p, &mv[i], &u);
        int s = -quiesce(p, -beta, -alpha, qply + 1);
        unmake(p, &mv[i], &u);
        if (g_aborted) return alpha;
        if (s >= beta) return beta;
        if (s > alpha) alpha = s;
    }
    return alpha;
}

static int negamax(chess_pos_t* p, int depth, int alpha, int beta, int ply) {
    if (depth <= 0) return quiesce(p, alpha, beta, 0);

    if (++g_nodes > g_cap) { g_aborted = 1; return alpha; }

    // Repetition along the current search path scores 0.
    uint64_t sig = pos_sig(p);
    for (int i = g_sig_n - 2; i >= 0; i -= 2)
        if (g_sigs[i] == sig) return 0;
    if (g_sig_n < (int)(sizeof(g_sigs) / sizeof(g_sigs[0])))
        g_sigs[g_sig_n++] = sig;

    int incheck = chess_in_check(p);
    if (incheck && ply < 60) depth++;          // check extension

    chess_move_t mv[CHESS_MAXMOVES];
    int sc[CHESS_MAXMOVES];
    int n = chess_gen(p, mv);
    if (n == 0) {
        g_sig_n--;
        return incheck ? -(MATE - ply) : 0;
    }

    score_moves(p, mv, sc, n);
    order_moves(mv, sc, n);

    int best = -INF;
    for (int i = 0; i < n; i++) {
        undo_t u;
        make(p, &mv[i], &u);
        int s = -negamax(p, depth - 1, -beta, -alpha, ply + 1);
        unmake(p, &mv[i], &u);
        if (g_aborted) { g_sig_n--; return best > -INF ? best : alpha; }
        if (s > best) best = s;
        if (s > alpha) alpha = s;
        if (alpha >= beta) break;              // fail-high
    }
    if (g_sig_n > 0) g_sig_n--;                // pop (make may have skipped)
    return best;
}

void chess_search(const chess_pos_t* pos, int max_depth,
                  uint32_t node_cap, chess_result_t* out) {
    init_tables(); init_masks();
    chess_pos_t p = *pos;

    out->nodes = 0; out->depth = 0; out->score = 0;
    out->mate = 0;  out->best.from = 0; out->best.to = 0;
    out->best.promo = 0; out->best.flags = 0;

    chess_move_t root[CHESS_MAXMOVES];
    int rsc[CHESS_MAXMOVES];
    int n = chess_gen(&p, root);
    if (n == 0) return;                        // caller checks game state
    out->best = root[0];                       // legal fallback

    g_nodes = 0; g_cap = node_cap ? node_cap : 1u << 30;
    g_aborted = 0; g_sig_n = 0;
    score_moves(&p, root, rsc, n);
    order_moves(root, rsc, n);

    int best_score = 0, completed = 0;
    for (int d = 1; d <= max_depth && d < 64; d++) {
        int alpha = -INF, beta = INF;
        chess_move_t iter_best = root[0];
        int iter_score = -INF;

        int i;
        for (i = 0; i < n; i++) {
            undo_t u;
            make(&p, &root[i], &u);
            int s = -negamax(&p, d - 1, -beta, -alpha, 1);
            unmake(&p, &root[i], &u);
            if (g_aborted) break;
            if (s > iter_score) { iter_score = s; iter_best = root[i]; }
            if (s > alpha) alpha = s;
        }

        if (g_aborted && i < n) break;         // discard partial iteration
        // Completed this depth.
        best_score = iter_score;
        out->best   = iter_best;
        out->depth  = d;
        out->score  = best_score;
        out->mate   = (best_score > MATE - MATE_BOND ||
                       best_score < -(MATE - MATE_BOND));
        completed   = 1;
        promote_best(root, rsc, n, &iter_best);
        if (out->mate) break;                  // forced mate found — stop
    }

    if (!completed) out->depth = 0;            // even d=1 didn't finish
    out->nodes = (uint32_t)(g_nodes > 0xFFFFFFFFu ? 0xFFFFFFFFu : g_nodes);
}

// ─────────────────────────────────────────────────────────────────
// Game state / draws
// ─────────────────────────────────────────────────────────────────
static int insufficient_material(const chess_pos_t* p) {
    int minors[2] = { 0, 0 }, bishops[2] = { 0, 0 };
    int bcol[2] = { -1, -1 };
    for (int s = 0; s < 64; s++) {
        int8_t pc = p->board[s];
        if (!pc) continue;
        int t = pc > 0 ? pc : -pc;
        int side = pc > 0 ? 0 : 1;
        if (t == PAWN || t == ROOK || t == QUEEN) return 0;
        if (t == KNIGHT || t == BISHOP) {
            minors[side]++;
            if (t == BISHOP) {
                bishops[side]++;
                int col = (RNK(s) + FIL(s)) & 1;
                if (bcol[side] == -1) bcol[side] = col;
                else if (bcol[side] != col) return 0;   // opposite-color B
            }
        }
    }
    if (minors[0] + minors[1] <= 1) return 1;            // K vs K / K+minor
    if (minors[0] == bishops[0] && minors[1] == bishops[1] &&
        bishops[0] <= 1 && bishops[1] <= 1 && bcol[0] == bcol[1]) return 1;
    return 0;
}

int chess_game_state(const chess_pos_t* p) {
    chess_move_t mv[CHESS_MAXMOVES];
    int n = chess_gen(p, mv);
    if (n == 0) return chess_in_check(p) ? 1 : 2;
    if (p->halfmove >= 100) return 3;
    if (insufficient_material(p)) return 3;
    return 0;
}

// ─────────────────────────────────────────────────────────────────
// Setup / notation helpers
// ─────────────────────────────────────────────────────────────────
void chess_new(chess_pos_t* p) {
    static const int8_t back[8] = { ROOK, KNIGHT, BISHOP, QUEEN,
                                    KING, BISHOP, KNIGHT, ROOK };
    for (int i = 0; i < 64; i++) p->board[i] = 0;
    for (int f = 0; f < 8; f++) {
        p->board[f]      = (int8_t)back[f];          // white back rank
        p->board[8 + f]  = P_WP;
        p->board[48 + f] = -PAWN;
        p->board[56 + f] = (int8_t)-back[f];
    }
    p->side = WHITE;
    p->castle = 15;
    p->ep = -1;
    p->halfmove = 0;
    p->ply = 0;
    p->ksq[0] = 4;
    p->ksq[1] = 60;
}

static int fen_piece(char c) {
    switch (c) {
      case 'P': return P_WP;  case 'N': return P_WN;  case 'B': return P_WB;
      case 'R': return P_WR;  case 'Q': return P_WQ;  case 'K': return P_WK;
      case 'p': return -PAWN; case 'n': return -KNIGHT;
      case 'b': return -BISHOP; case 'r': return -ROOK;
      case 'q': return -QUEEN;  case 'k': return -KING;
      default:  return 0;
    }
}

int chess_fen(chess_pos_t* p, const char* fen) {
    init_tables(); init_masks();
    for (int i = 0; i < 64; i++) p->board[i] = 0;
    p->ep = -1; p->castle = 0; p->halfmove = 0; p->ply = 0;
    p->ksq[0] = p->ksq[1] = -1;

    int sq = 56, i = 0;                          // FEN starts at a8
    for (; fen[i] && fen[i] != ' '; i++) {
        char c = fen[i];
        if (c == '/') { sq -= 16; continue; }
        if (c >= '1' && c <= '8') { sq += c - '0'; continue; }
        int pc = fen_piece(c);
        if (!pc || sq < 0 || sq > 63) return -1;
        p->board[sq++] = (int8_t)pc;
        if (pc == P_WK) p->ksq[0] = (int8_t)(sq - 1);
        if (pc == -KING) p->ksq[1] = (int8_t)(sq - 1);
    }
    if (p->ksq[0] < 0 || p->ksq[1] < 0) return -1;

    if (fen[i] == ' ') i++;
    if (fen[i] == 'w') p->side = WHITE;
    else if (fen[i] == 'b') p->side = BLACK;
    else return -1;
    i++;

    if (fen[i] == ' ') i++;
    if (fen[i] == '-') i++;
    else for (; fen[i] && fen[i] != ' '; i++) {
        switch (fen[i]) {
          case 'K': p->castle |= 1; break;
          case 'Q': p->castle |= 2; break;
          case 'k': p->castle |= 4; break;
          case 'q': p->castle |= 8; break;
          default: return -1;
        }
    }

    if (fen[i] == ' ') i++;
    if (fen[i] == '-') { i++; }
    else if (fen[i] >= 'a' && fen[i] <= 'h' && fen[i+1] >= '1' && fen[i+1] <= '8') {
        p->ep = (int8_t)((fen[i+1] - '1') * 8 + (fen[i] - 'a'));
        i += 2;
    }

    // Halfmove/fullmove are optional (tests often pass 4 fields).
    if (fen[i] == ' ') {
        i++;
        int v = 0; int got = 0;
        while (fen[i] >= '0' && fen[i] <= '9') { v = v*10 + (fen[i]-'0'); i++; got = 1; }
        if (got) p->halfmove = (int16_t)v;
    }
    return 0;
}

void chess_sq_name(int sq, char out[3]) {
    out[0] = (char)('a' + FIL(sq));
    out[1] = (char)('1' + RNK(sq));
    out[2] = 0;
}

int chess_move_eq(const chess_move_t* a, const chess_move_t* b) {
    return a->from == b->from && a->to == b->to &&
           a->promo == b->promo && a->flags == b->flags;
}

void chess_move_name(const chess_move_t* m, char out[8]) {
    if (m->flags & MV_CASTLE) {
        out[0] = 'O'; out[1] = '-'; out[2] = 'O';
        int n = 3;
        if (m->to < m->from) { out[n++] = '-'; out[n++] = 'O'; }
        out[n] = 0;
        return;
    }
    chess_sq_name(m->from, out);
    int n = 2;
    chess_sq_name(m->to, out + 2);
    n = 4;
    if (m->promo) {
        static const char pc[7] = { 0, 0, 'n', 'b', 'r', 'q', 0 };
        out[n++] = pc[m->promo];
    }
    out[n] = 0;
}

int chess_parse_move(const chess_pos_t* p, const char* s, chess_move_t* m) {
    if (!s || !s[0] || !s[1] || !s[2] || !s[3]) return -1;
    int ff = s[0] - 'a', fr = s[1] - '1', tf = s[2] - 'a', tr = s[3] - '1';
    if (ff < 0 || ff > 7 || fr < 0 || fr > 7 ||
        tf < 0 || tf > 7 || tr < 0 || tr > 7) return -1;
    int from = fr * 8 + ff, to = tr * 8 + tf;

    chess_move_t mv[CHESS_MAXMOVES];
    int n = chess_gen(p, mv);
    uint8_t promo = 0;
    if (s[4]) {
        char c = (char)(s[4] | 0x20);
        if      (c == 'q') promo = QUEEN;
        else if (c == 'r') promo = ROOK;
        else if (c == 'b') promo = BISHOP;
        else if (c == 'n') promo = KNIGHT;
        else return -1;
    }
    for (int i = 0; i < n; i++) {
        if (mv[i].from == from && mv[i].to == to && mv[i].promo == promo) {
            *m = mv[i];
            return 0;
        }
    }
    return -1;
}

uint64_t chess_sig(const chess_pos_t* p) { return pos_sig(p); }

void chess_apply(chess_pos_t* p, const chess_move_t* m) {
    undo_t u;
    make(p, m, &u);
    (void)u;
}

// ─────────────────────────────────────────────────────────────────
// Perft — movegen correctness probe (used by the host selftest).
// ─────────────────────────────────────────────────────────────────
uint64_t chess_perft(const chess_pos_t* pos, int depth) {
    chess_pos_t p = *pos;
    if (depth == 0) return 1;
    chess_move_t mv[CHESS_MAXMOVES];
    int n = chess_gen(&p, mv);
    if (depth == 1) return (uint64_t)n;
    uint64_t total = 0;
    for (int i = 0; i < n; i++) {
        undo_t u;
        make(&p, &mv[i], &u);
        total += chess_perft(&p, depth - 1);
        unmake(&p, &mv[i], &u);
    }
    return total;
}
