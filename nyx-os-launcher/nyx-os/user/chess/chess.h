// user/chess/chess.h — Chess for Nyx OS: shared types + engine API.
//
// Design notes (per project spec):
//   - Board is ONE contiguous array of 64 signed bytes.  Sign = side
//     (white positive, black negative), magnitude = piece type
//     1..6 = P N B R Q K.  Simple, debuggable, cache-friendly.
//   - Index 0 = a1 (white's back rank), index 63 = h8.  White pawns
//     step +8.  rank = sq/8, file = sq%8.
//   - No libc: engine uses only <stdint.h>.  The UI layer talks to the
//     kernel through the same ring-3 syscalls DOOM.EXE uses.
//
// Turn order: WHITE is the human, BLACK is the computer (AI).  The UI
// layer owns the game history (full position snapshots — cheap, ~80 B
// each) so "undo" never needs engine cooperation.

#ifndef CHESS_H
#define CHESS_H

#include <stdint.h>

// ── Piece codes ──────────────────────────────────────────────────
enum { P_EMPTY = 0,
       P_WP = 1, P_WN = 2, P_WB = 3, P_WR = 4, P_WQ = 5, P_WK = 6 };
#define PAWN   1
#define KNIGHT 2
#define BISHOP 3
#define ROOK   4
#define QUEEN  5
#define KING   6
#define WHITE  1
#define BLACK  (-1)

// ── Move flags ───────────────────────────────────────────────────
#define MV_CAP    1     // capture (incl. en passant)
#define MV_DPP    2     // pawn double push (sets ep square)
#define MV_EP     4     // en passant capture
#define MV_CASTLE 8     // castling move

#define CHESS_MAXMOVES 256

typedef struct {
    uint8_t from, to;
    uint8_t promo;      // 0 = none, else KNIGHT..QUEEN
    uint8_t flags;
} chess_move_t;

// Full game position (~80 B — snapshots for undo are affordable).
typedef struct {
    int8_t  board[64];
    int8_t  side;       // +1 white to move, -1 black
    uint8_t castle;     // bit0 White-O-O  bit1 White-O-O-O  bit2/3 black
    int8_t  ep;         // en-passant target square, -1 = none
    int16_t halfmove;   // 50-move-draw clock
    int16_t ply;        // plies since game start
    int8_t  ksq[2];     // king squares: [0] white, [1] black
} chess_pos_t;

// Search result.
typedef struct {
    uint32_t     nodes;
    int          depth;   // deepest FULLY completed iteration
    int          score;   // centipawns, from the SEARCHING side's view
    int          mate;    // 1 = best line delivers forced mate
    chess_move_t best;
} chess_result_t;

// ── Engine API ───────────────────────────────────────────────────
void     chess_new(chess_pos_t* p);
int      chess_fen(chess_pos_t* p, const char* fen);      // 0 ok, -1 bad

// Generate LEGAL moves.  Returns count (0 = no legal moves: mate or
// stalemate — disambiguate with chess_in_check).
int      chess_gen(const chess_pos_t* p, chess_move_t* out);

int      chess_in_check(const chess_pos_t* p);            // side to move

// 0 = playing, 1 = checkmate (side to move lost), 2 = stalemate,
// 3 = draw (50-move / repetition / insufficient material).
int      chess_game_state(const chess_pos_t* p);

// Fixed-depth search with a hard node budget (keeps the UI freeze
// bounded).  out->best is always a legal move when legal moves exist.
void     chess_search(const chess_pos_t* p, int max_depth,
                      uint32_t node_cap, chess_result_t* out);

// ── Test / debug helpers ─────────────────────────────────────────
uint64_t chess_perft(const chess_pos_t* p, int depth);
void     chess_sq_name(int sq, char out[3]);              // "a1".."h8"
int      chess_parse_move(const chess_pos_t* p, const char* s,
                          chess_move_t* m);               // "e2e4","e7e8q"
void     chess_move_name(const chess_move_t* m, char out[8]); // "e2e4"/"e7e8q"
int      chess_move_eq(const chess_move_t* a, const chess_move_t* b);
uint64_t chess_sig(const chess_pos_t* p);   // position hash (repetition)

// Apply a legal move to a position (UI uses it for real game moves;
// the engine uses its internal make/unmake for search).
void     chess_apply(chess_pos_t* p, const chess_move_t* m);

#endif // CHESS_H
