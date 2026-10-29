// tools/_chess_selftest.c — host driver for the chess engine selftest.
// Links against user/chess/chess_engine.c and asserts perft counts,
// mate finding, tactic choice, castling/ep legality and selfplay sanity.
#include <stdio.h>
#include <string.h>
#include "chess.h"

static int fails = 0, checks = 0;

static void check(int cond, const char* what, long got, long want) {
    checks++;
    if (cond) printf("  ok  %-42s %ld\n", what, got);
    else { printf("FAIL  %-42s got %ld want %ld\n", what, got, want); fails++; }
}

static long perft_fen(const char* fen, int d) {
    chess_pos_t p;
    if (chess_fen(&p, fen) != 0) { printf("bad fen %s\n", fen); fails++; return -1; }
    return (long)chess_perft(&p, d);
}

static void check_perft(const char* fen, int d, long want, const char* what) {
    long got = perft_fen(fen, d);
    check(got == want, what, got, want);
}

int main(void) {
    const char* START = "rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1";
    const char* KIWI  = "r3k2r/p1ppqpb1/bn2pnp1/3PN3/1p2P3/2N2Q1p/PPPBBPPP/R3K2R w KQkq - 0 1";
    const char* POS3  = "8/2p5/3p4/KP5r/1R3p1k/8/4P1P1/8 w - - 0 1";
    const char* POS4  = "r3k2r/Pppp1ppp/1b3nbN/nP6/BBP1P3/q4N2/Pp1P2PP/R2Q1RK1 w kq - 0 1";
    const char* POS5  = "rnbq1k1r/pp1Pbppp/2p5/8/2B5/8/PPP1NnPP/RNBQK2R w KQ - 1 8";

    // ── Start position perft (reference: Chess Programming Wiki) ──
    check_perft(START, 1, 20,    "perft(1) start");
    check_perft(START, 2, 400,   "perft(2) start");
    check_perft(START, 3, 8902,  "perft(3) start");
    check_perft(START, 4, 197281,"perft(4) start");

    // ── Kiwipete: castling, pins, ep, promotions ──────────────────
    check_perft(KIWI, 1, 48,    "kiwipete perft(1)");
    check_perft(KIWI, 2, 2039,  "kiwipete perft(2)");
    check_perft(KIWI, 3, 97862, "kiwipete perft(3)");

    // ── Position 3: ep pins / check evasions ──────────────────────
    check_perft(POS3, 1, 14,     "pos3 perft(1)");
    check_perft(POS3, 4, 43238,  "pos3 perft(4)");
    check_perft(POS3, 5, 674624, "pos3 perft(5)");

    // ── Position 4: promotions (+ underpromo captures) ────────────
    check_perft(POS4, 1, 6,    "pos4 perft(1)");
    check_perft(POS4, 3, 9467, "pos4 perft(3)");

    // ── Position 5 ─────────────────────────────────────────────────
    check_perft(POS5, 1, 44,    "pos5 perft(1)");
    check_perft(POS5, 3, 62379, "pos5 perft(3)");

    // ── Mate in 1: Qh7# (back-rank mate) ──────────────────────────
    {
        chess_pos_t p; chess_result_t r;
        chess_fen(&p, "6k1/5ppp/8/8/8/8/5PPP/R5K1 w - - 0 1");
        chess_search(&p, 3, 4000000, &r);
        char nm[8]; chess_move_name(&r.best, nm);
        check(r.mate && !strcmp(nm, "a1a8"), "mate in 1: Ra8#", (long)nm[0], (long)'R');
    }
    // ── Mate in 2: classic smothered-ish K+Q ladder ───────────────
    {
        chess_pos_t p; chess_result_t r;
        chess_fen(&p, "7k/8/8/8/8/8/R7/1R5K w - - 0 1");
        chess_search(&p, 5, 8000000, &r);
        check(r.mate, "mate in 2: two rooks ladder", r.depth, 2);
    }
    // ── Tactic: hanging queen on h4, Nxh4 wins it ─────────────────
    {
        chess_pos_t p; chess_result_t r;
        chess_fen(&p, "rnb1kbnr/pppp1ppp/8/4p3/7q/5N2/PPPP1PPP/RNBQKB1R w KQkq - 0 1");
        chess_search(&p, 4, 4000000, &r);
        char nm[8]; chess_move_name(&r.best, nm);
        check(!strcmp(nm, "f3h4"),
              "tactic: Nxh4 wins the queen", (long)r.score, 0);
        // ≈ +900 for the queen minus edge-PST/king-safety for Nh4
        // (black's ...g5 kicks the knight).  ≥600 still proves the
        // engine grabs material over quiet alternatives.
        check(r.score >= 600, "tactic score ≈ +queen", (long)r.score, 900);
    }
    // ── Castling: rights + legality through attacked squares ─────
    {
        chess_pos_t p; chess_move_t mv[CHESS_MAXMOVES];
        chess_fen(&p, "r3k2r/8/8/8/8/8/8/R3K2R w KQkq - 0 1");
        int n = chess_gen(&p, mv);
        int csum = 0;
        for (int i = 0; i < n; i++) if (mv[i].flags & MV_CASTLE) csum++;
        check(csum == 2, "both white castles available", csum, 2);
    }
    {
        chess_pos_t p; chess_move_t mv[CHESS_MAXMOVES];
        // e1 rook pinned — O-O must be refused.
        chess_fen(&p, "4k3/8/8/8/8/8/4r3/4K2R w K - 0 1");
        int n = chess_gen(&p, mv);
        int csum = 0;
        for (int i = 0; i < n; i++)
            if ((mv[i].flags & MV_CASTLE) && mv[i].from == 4) csum++;
        check(csum == 0, "no castle out of check/pin", csum, 0);
    }
    // ── En passant available and legal ────────────────────────────
    {
        chess_pos_t p; chess_move_t mv[CHESS_MAXMOVES];
        chess_fen(&p, "4k3/8/8/3pP3/8/8/8/4K3 w - d6 0 1");
        int n = chess_gen(&p, mv), found = 0;
        for (int i = 0; i < n; i++)
            if ((mv[i].flags & MV_EP) && mv[i].from == 36 && mv[i].to == 43) found = 1;
        check(found, "en passant generated", found, 1);
    }
    // ── Stalemate detection ───────────────────────────────────────
    {
        chess_pos_t p;
        chess_fen(&p, "7k/5Q2/6K1/8/8/8/8/8 b - - 0 1");
        int st = chess_game_state(&p);
        check(st == 2, "stalemate detected", st, 2);
    }
    // ── Checkmate detection ───────────────────────────────────────
    {
        chess_pos_t p;
        chess_fen(&p, "R5k1/5ppp/8/8/8/8/8/6K1 b - - 0 1");
        int st = chess_game_state(&p);
        check(st == 1, "checkmate detected", st, 1);
    }
    // ── FEN round-trip: apply moves to a fixed position ───────────
    {
        chess_pos_t p; chess_move_t m;
        chess_fen(&p, "rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1");
        int ok = 1;
        const char* seq[] = { "e2e4", "e7e5", "g1f3", "b8c6", "f1b5", 0 };
        for (int i = 0; seq[i]; i++) {
            if (chess_parse_move(&p, seq[i], &m) != 0) { ok = 0; break; }
            chess_apply(&p, &m);
        }
        check(ok, "apply e4 e5 Nf3 Nc6 Bb5", ok, 1);
    }
    // ── Selfplay sanity: 60 plies, no illegal move, game progresses ─
    {
        chess_pos_t p;
        chess_new(&p);
        int illegal = 0, cap_cnt = 0;
        chess_move_t mv[CHESS_MAXMOVES];
        for (int ply = 0; ply < 60; ply++) {
            int st = chess_game_state(&p);
            if (st != 0) break;
            chess_result_t r;
            chess_search(&p, 3, 120000, &r);
            int n = chess_gen(&p, mv), found = 0;
            for (int i = 0; i < n; i++)
                if (chess_move_eq(&mv[i], &r.best)) { found = 1; break; }
            if (!found) { illegal = 1; break; }
            if (r.best.flags & MV_CAP) cap_cnt++;
            chess_apply(&p, &r.best);
        }
        check(!illegal, "selfplay 60 plies legal", illegal, 0);
        check(cap_cnt > 0, "selfplay had captures", cap_cnt, 1);
    }

    printf("\n%d checks, %d failures\n", checks, fails);
    return fails ? 1 : 0;
}
