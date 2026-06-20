#include <iostream>
#include <sstream>
#include <cstdint>
#include <cstdlib>

#include "./state.hpp"
#include "config.hpp"
#include "../../policy/game_history.hpp"


/*============================================================
 * KP (King-Piece) Evaluation tables
 *
 * Always compiled. Toggled at runtime via use_kp_eval param.
 *============================================================*/

// KP material (10x scale for fine positional granularity)
static const int kp_material[7] = {0, 20, 60, 70, 80, 200, 1000};

// Material-only (simple scale)
static const int simple_material[7] = {0, 2, 6, 7, 8, 20, 100};

// Piece-Square Tables (white perspective, mirror for black)
static const int pst[6][BOARD_H][BOARD_W] = {
    // Pawn
    {{ 0,  0,  0,  0,  0}, {15, 15, 15, 15, 15}, { 4,  6, 10,  6,  4},
     { 2,  4,  6,  4,  2}, { 0,  2,  2,  2,  0}, { 0,  0,  0,  0,  0}},
    // Rook
    {{ 2,  2,  2,  2,  2}, { 4,  4,  4,  4,  4}, { 0,  0,  2,  0,  0},
     { 0,  0,  2,  0,  0}, { 0,  0,  2,  0,  0}, { 0,  0,  0,  0,  0}},
    // Knight
    {{-4, -2,  0, -2, -4}, {-2,  2,  4,  2, -2}, { 0,  4,  6,  4,  0},
     { 0,  4,  6,  4,  0}, {-2,  2,  4,  2, -2}, {-4, -2,  0, -2, -4}},
    // Bishop
    {{-2,  0,  0,  0, -2}, { 0,  3,  4,  3,  0}, { 0,  4,  4,  4,  0},
     { 0,  4,  4,  4,  0}, { 0,  3,  4,  3,  0}, {-2,  0,  0,  0, -2}},
    // Queen
    {{-2,  0,  2,  0, -2}, { 0,  2,  4,  2,  0}, { 0,  4,  6,  4,  0},
     { 0,  4,  6,  4,  0}, { 0,  2,  4,  2,  0}, {-2,  0,  2,  0, -2}},
    // King
    {{-8, -8, -8, -8, -8}, {-4, -4, -4, -4, -4}, {-4, -4, -4, -4, -4},
     {-4, -4, -4, -4, -4}, { 4,  4,  0,  4,  4}, { 6,  6,  2,  6,  6}},
};

// King tropism weights
static const int tropism_w[7] = {0, 0, 3, 3, 2, 5, 0};

static int king_tropism(
    int piece_type,
    int pr, int pc,
    int ekr, int ekc
){
    int dist = std::max(std::abs(pr - ekr), std::abs(pc - ekc));
    if(dist <= 2){
        return tropism_w[piece_type] * (3 - dist);
    }
    return 0;
}


//評估 
// 額外的位置性評估項：weak/strong 的評估沒有、但對棋力很有幫助。
// 全部只對「單一方」計算，evaluate() 會對雙方各算一次再相減，保持對稱。

// 通行兵（passed pawn）：依推進程度給分，index = 推進格數（越大越接近升變）。
// 小棋盤上「升變威脅」常常是決勝關鍵，所以越靠近升變分數越高。
static const int passed_pawn_bonus[BOARD_H] = {0, 0, 6, 14, 28, 55};

// 回傳 side 方的位置性加分。color：0=白（往 row 變小推進），1=黑（往 row 變大）。
static int positional_extras(
    const char side[BOARD_H][BOARD_W],
    const char opp[BOARD_H][BOARD_W],
    int color
){
    int bonus = 0;
    int bishops = 0;
    int file_pawns[BOARD_W] = {0};

    for(int r = 0; r < BOARD_H; r++){
        for(int c = 0; c < BOARD_W; c++){
            int p = side[r][c];
            if(!p) continue;
            if(p == 4){ bishops++; }            // 主教
            if(p == 1){                         // 兵
                file_pawns[c]++;
                int adv = (color == 0) ? (BOARD_H - 1 - r) : r;  // 推進程度
                // 通行兵判定：同列與左右相鄰列、在它前方都沒有敵兵
                bool passed = true;
                for(int dc = -1; dc <= 1 && passed; dc++){
                    int fc = c + dc;
                    if(fc < 0 || fc >= BOARD_W){ continue; }
                    for(int rr = 0; rr < BOARD_H; rr++){
                        bool ahead = (color == 0) ? (rr < r) : (rr > r);
                        if(ahead && opp[rr][fc] == 1){ passed = false; break; }
                    }
                }
                if(passed){ bonus += passed_pawn_bonus[adv]; }
            }
        }
    }

    if(bishops >= 2){ bonus += 15; }            // 主教對

    for(int c = 0; c < BOARD_W; c++){           // 疊兵：同列多兵扣分
        if(file_pawns[c] > 1){ bonus -= 8 * (file_pawns[c] - 1); }
    }

    return bonus;
}

// 王翼兵盾：王正前方三格的己方兵，每個 +8（王越有兵掩護越安全）。
static int king_shield(
    const char side[BOARD_H][BOARD_W],
    int kr, int kc, int color
){
    if(kr < 0){ return 0; }
    int front = (color == 0) ? (kr - 1) : (kr + 1);
    if(front < 0 || front >= BOARD_H){ return 0; }
    int s = 0;
    for(int dc = -1; dc <= 1; dc++){
        int c = kc + dc;
        if(c < 0 || c >= BOARD_W){ continue; }
        if(side[front][c] == 1){ s += 8; }
    }
    return s;
}
//評估




/*============================================================
 * evaluate() — runtime-selectable eval strategy
 *============================================================*/

int State::evaluate(
    bool use_kp_eval,
    bool use_mobility,
    const GameHistory* history
){
    (void)history; // just to suppress warning

    // [ Hackathon TODO 1-1 ]
    // if in win state, return max score(you can check base_state.hpp for max score)
    if (this->game_state== WIN)  //優先執行
        return P_MAX;
    auto self_board = this->board.board[this->player];
    auto oppn_board = this->board.board[1 - this->player];
    int self_score = 0, oppn_score = 0;

    if(use_kp_eval){
        /* === KP eval: material + PST + tropism === */

        int self_kr = -1, self_kc = -1;
        int oppn_kr = -1, oppn_kc = -1;
        // [ Hackathon TODO 1-3 ]
        // get the position for player's king and opponent's king在整個棋盤裡找 king 的座標 (row, col)
        for(int r = 0; r < BOARD_H; r++){
            for(int c = 0; c < BOARD_W; c++){
                if(self_board[r][c] == 6){
                    self_kr = r; self_kc = c;
                }
                if(oppn_board[r][c] == 6){
                    oppn_kr = r; oppn_kc = c;
                }
                if(self_kr != -1 && oppn_kr != -1) break;
            }
            if(self_kr != -1 && oppn_kr != -1) break;
        }

        // [ Hackathon TODO 1-4 ]
        // sum player/opponent pieces' value and add to score把自己/對手所有棋子加總分數
        // if enemy king is still on the board, you should also call king_tropism for your pieces and add the value to score算棋子分數 + 如果敵方 king 還在，就加「靠近 king 的攻擊分數」
        // king_tropism is already given above
        // iterate self pieces
        for(int r = 0; r < BOARD_H; r++){
            for(int c = 0; c < BOARD_W; c++){
                int p = self_board[r][c];
                if(!p) continue;
        
                self_score += kp_material[p];

                // 棋子位置表（白方視角）
                int pst_idx = p - 1; // 把棋子種類轉成 PST 的 index
                int pl = this->player; // 這些棋子的顏色（0=白，1=黑）
                int pst_r = (pl == 0) ? r : (BOARD_H - 1 - r);//翻棋盤
                self_score += pst[pst_idx][pst_r][c];//位置價值
                // 若敵方國王存在，加入對敵方國王的吸引力分
                if(oppn_kr != -1){
                    self_score += king_tropism(p, r, c, oppn_kr, oppn_kc);
                }
            }
        }

        // 遍歷對手棋子
        for(int r = 0; r < BOARD_H; r++){
            for(int c = 0; c < BOARD_W; c++){
                int p = oppn_board[r][c];
                if(!p) continue;
                oppn_score += kp_material[p];
                int pst_idx = p - 1;
                int pl = 1 - this->player; // 對手棋子的顏色
                int pst_r = (pl == 0) ? r : (BOARD_H - 1 - r);
                oppn_score += pst[pst_idx][pst_r][c];
                // 若我方國王存在，加入對我方國王的吸引力分
                if(self_kr != -1){
                    oppn_score += king_tropism(p, r, c, self_kr, self_kc);
                }
            }
        }

        //評估 位置性額外項（雙方對稱計算）+ 王翼兵盾
        self_score += positional_extras(self_board, oppn_board, this->player);
        oppn_score += positional_extras(oppn_board, self_board, 1 - this->player);
        self_score += king_shield(self_board, self_kr, self_kc, this->player);
        oppn_score += king_shield(oppn_board, oppn_kr, oppn_kc, 1 - this->player);

    }else{
        /* === Simple material-only eval === */

        // [ Hackathon TODO 1-2 ]
        // Simply add each piece's value to score 把每個格子的棋子轉成分數累加
        for(int r = 0; r < BOARD_H; r++){
            for(int c = 0; c < BOARD_W; c++){
                self_score += simple_material[static_cast<int>(self_board[r][c])];
                oppn_score += simple_material[static_cast<int>(oppn_board[r][c])];
            }
        }

    }

    int bonus = 0;

    /* === Mobility bonus === */
    if(use_mobility){
        // [ Hackathon TODO 1-5 ]
        // you can calculate mobility by legal actions size
        // bonus += 2 * (self_mobility - oppn_mobility);
        // 計算加分項公式bonus += 2 * (self_mobility - oppn_mobility)
        // 先算我現在可以走哪些合法步
        this->get_legal_actions();
        int self_mobility = static_cast<int>(this->legal_actions.size());

        // 建立一個 "null" 狀態來評估對手的 legal actions
        BaseState* ns = this->create_null_state();
        int oppn_mobility = 0;
        if(ns){
            State* s_ns = static_cast<State*>(ns);
            oppn_mobility = static_cast<int>(s_ns->legal_actions.size());
            delete ns;
        }

        bonus += 2 * (self_mobility - oppn_mobility);

    }

    //評估 新增：先手（tempo）小加分——輪到自己走本身就是一點優勢
    bonus += 6;

    //材料 
    // 對齊評分規則：分不出勝負時，到 MAX_STEP(=100手) 是「比純子力」決勝。
    // 因此 step 越接近 MAX_STEP，就越加重「純子力差」，讓引擎主動搶子、
    // 守住子力領先，而不是追求對最終判定無用的位置分。
    // 第 ~40 步前幾乎無影響；會被拖長的局面（正是打 weak 那種）越來越主導。
    if(use_kp_eval){
        int self_mat = 0, oppn_mat = 0;
        for(int r = 0; r < BOARD_H; r++){
            for(int c = 0; c < BOARD_W; c++){
                self_mat += kp_material[(int)self_board[r][c]];
                oppn_mat += kp_material[(int)oppn_board[r][c]];
            }
        }
        int mat_diff = self_mat - oppn_mat;
        int sf = this->step - 40;
        if(sf < 0){ sf = 0; }
        if(sf > MAX_STEP - 40){ sf = MAX_STEP - 40; }
        bonus += mat_diff * 3 * sf / (MAX_STEP - 40);  // step→MAX_STEP 時子力差再 ×3

    }
    //材料 

    return self_score - oppn_score + bonus;
}



/*============================================================
 * Zobrist hash for transposition table
 *============================================================*/
static uint64_t zobrist_piece[2][7][BOARD_H][BOARD_W];
static uint64_t zobrist_side;
static bool zobrist_ready = false;

static void init_zobrist(){
    uint64_t s = 0x7A35C9D1E4F02B68ULL;
    auto rand64 = [&s]() -> uint64_t {
        s ^= s << 13; s ^= s >> 7; s ^= s << 17; return s;
    };
    for(int p = 0; p < 2; p++){
        for(int t = 0; t < 7; t++){
            for(int r = 0; r < BOARD_H; r++){
                for(int c = 0; c < BOARD_W; c++){
                    zobrist_piece[p][t][r][c] = rand64();
                }
            }
        }
    }
    zobrist_side = rand64();
    zobrist_ready = true;
}

uint64_t State::compute_hash_full() const{
    if(!zobrist_ready){
        init_zobrist();
    }
    uint64_t h = 0;
    for(int p = 0; p < 2; p++){
        for(int r = 0; r < BOARD_H; r++){
            for(int c = 0; c < BOARD_W; c++){
                int piece = this->board.board[p][r][c];
                if(piece){
                    h ^= zobrist_piece[p][piece][r][c];
                }
            }
        }
    }
    if(this->player){
        h ^= zobrist_side;
    }
    return h;
}


/**
 * @brief return next state after the move
 *
 * @param move
 * @return State*
 */
State* State::next_state(const Move& move){
    if(!zobrist_ready){ init_zobrist(); }

    Board next = this->board;
    Point from = move.first, to = move.second;
    int p = this->player;
    int opp = 1 - p;

    int8_t orig_piece = next.board[p][from.first][from.second];
    int8_t moved = orig_piece;
    //promotion for pawn
    if(moved == 1 && (to.first==BOARD_H-1 || to.first==0)){
        moved = 5;
    }

    /* Incremental hash update */
    uint64_t h = this->hash();
    h ^= zobrist_side;  /* toggle side to move */

    /* XOR out piece from source */
    h ^= zobrist_piece[p][orig_piece][from.first][from.second];

    /* XOR out captured piece at destination */
    int8_t captured = next.board[opp][to.first][to.second];
    if(captured){
        h ^= zobrist_piece[opp][captured][to.first][to.second];
        next.board[opp][to.first][to.second] = 0;
    }

    /* XOR in piece at destination */
    h ^= zobrist_piece[p][moved][to.first][to.second];

    next.board[p][from.first][from.second] = 0;
    next.board[p][to.first][to.second] = moved;

    State* ns = new State(next, opp);
    ns->step = this->step + 1;   //材料 修正：傳遞步數，讓搜尋樹知道距離 MAX_STEP 多遠
    ns->zobrist_hash = h;
    ns->zobrist_valid = true;
    return ns;
}


static const int move_table_rook_bishop[8][7][2] = {
  {{0, 1}, {0, 2}, {0, 3}, {0, 4}, {0, 5}, {0, 6}, {0, 7}},
  {{0, -1}, {0, -2}, {0, -3}, {0, -4}, {0, -5}, {0, -6}, {0, -7}},
  {{1, 0}, {2, 0}, {3, 0}, {4, 0}, {5, 0}, {6, 0}, {7, 0}},
  {{-1, 0}, {-2, 0}, {-3, 0}, {-4, 0}, {-5, 0}, {-6, 0}, {-7, 0}},
  {{1, 1}, {2, 2}, {3, 3}, {4, 4}, {5, 5}, {6, 6}, {7, 7}},
  {{1, -1}, {2, -2}, {3, -3}, {4, -4}, {5, -5}, {6, -6}, {7, -7}},
  {{-1, 1}, {-2, 2}, {-3, 3}, {-4, 4}, {-5, 5}, {-6, 6}, {-7, 7}},
  {{-1, -1}, {-2, -2}, {-3, -3}, {-4, -4}, {-5, -5}, {-6, -6}, {-7, -7}},
};

// [ Hackathon TODO 2-1 ]
// fill the knight move table
//馬的  8 種固定走法 L 形（row, col）
static const int move_table_knight[8][2] = {
    {1, 2}, {1, -2}, {-1, 2}, {-1, -2},
    {2, 1}, {2, -1}, {-2, 1}, {-2, -1},
};

static const int move_table_king[8][2] = {
  {1, 0}, {0, 1}, {-1, 0}, {0, -1}, 
  {1, 1}, {1, -1}, {-1, 1}, {-1, -1},
};


/*============================================================
 * Naive move generation (array-based, branch-heavy)
 *============================================================*/
void State::get_legal_actions_naive(){
    this->game_state = NONE;
    std::vector<Move> all_actions;
    all_actions.reserve(64);
    auto self_board = this->board.board[this->player];
    auto oppn_board = this->board.board[1 - this->player];

    int now_piece, oppn_piece;
    for(int i=0; i<BOARD_H; i+=1){
        for(int j=0; j<BOARD_W; j+=1){
            if((now_piece=self_board[i][j])){
                switch(now_piece){
                    case 1: //pawn
                        if(this->player && i<BOARD_H-1){
                            //black
                            if(!oppn_board[i+1][j] && !self_board[i+1][j]){
                                all_actions.push_back(Move(Point(i, j), Point(i+1, j)));
                            }
                            if(j<BOARD_W-1 && (oppn_piece=oppn_board[i+1][j+1])>0){
                                all_actions.push_back(Move(Point(i, j), Point(i+1, j+1)));
                                if(oppn_piece==6){
                                    this->game_state = WIN;
                                    this->legal_actions = all_actions;
                                    return;
                                }
                            }
                            if(j>0 && (oppn_piece=oppn_board[i+1][j-1])>0){
                                all_actions.push_back(Move(Point(i, j), Point(i+1, j-1)));
                                if(oppn_piece==6){
                                    this->game_state = WIN;
                                    this->legal_actions = all_actions;
                                    return;
                                }
                            }
                        }else if(!this->player && i>0){
                            //white
                            if(!oppn_board[i-1][j] && !self_board[i-1][j]){
                                all_actions.push_back(Move(Point(i, j), Point(i-1, j)));
                            }
                            if(j<BOARD_W-1 && (oppn_piece=oppn_board[i-1][j+1])>0){
                                all_actions.push_back(Move(Point(i, j), Point(i-1, j+1)));
                                if(oppn_piece==6){
                                    this->game_state = WIN;
                                    this->legal_actions = all_actions;
                                    return;
                                }
                            }
                            if(j>0 && (oppn_piece=oppn_board[i-1][j-1])>0){
                                all_actions.push_back(Move(Point(i, j), Point(i-1, j-1)));
                                if(oppn_piece==6){
                                    this->game_state = WIN;
                                    this->legal_actions = all_actions;
                                    return;
                                }
                            }
                        }
                        break;

                    case 2: //rook
                    case 4: //bishop
                    case 5: //queen
                        int st, end;
                        switch(now_piece){
                            case 2: st=0; end=4; break; //rook
                            case 4: st=4; end=8; break; //bishop
                            case 5: st=0; end=8; break; //queen
                            default: st=0; end=-1;
                        }
                        for(int part=st; part<end; part+=1){
                            auto move_list = move_table_rook_bishop[part];
                            for(int k=0; k<std::max(BOARD_H, BOARD_W); k+=1){
                                int p[2] = {move_list[k][0] + i, move_list[k][1] + j};

                                if(p[0]>=BOARD_H || p[0]<0 || p[1]>=BOARD_W || p[1]<0){
                                    break;
                                }
                                now_piece = self_board[p[0]][p[1]];
                                if(now_piece){
                                    break;
                                }

                                all_actions.push_back(Move(Point(i, j), Point(p[0], p[1])));

                                oppn_piece = oppn_board[p[0]][p[1]];
                                if(oppn_piece){
                                    if(oppn_piece==6){
                                        this->game_state = WIN;
                                        this->legal_actions = all_actions;
                                        return;
                                    }else{
                                        break;
                                    }
                                };
                            }
                        }
                        break;

                    case 3: //knight
                        // 騎士移動（8 個 L 形偏移）
                        // knight moves (8 L-shaped offsets)
                        for(int m = 0; m < 8; m++){
                            int p[2] = {i + move_table_knight[m][0], j + move_table_knight[m][1]};

                            if(p[0] >= BOARD_H || p[0] < 0 || p[1] >= BOARD_W || p[1] < 0){
                                continue;
                            }
                            now_piece = self_board[p[0]][p[1]];
                            if(now_piece){
                                continue;
                            }

                            all_actions.push_back(Move(Point(i, j), Point(p[0], p[1])));

                            oppn_piece = oppn_board[p[0]][p[1]];
                            if(oppn_piece==6){
                                this->game_state = WIN;
                                this->legal_actions = all_actions;
                                return;
                            }
                        }
                        break; ///至




                    case 6: //king
                        for(auto move: move_table_king){
                            int p[2] = {move[0] + i, move[1] + j};

                            if(p[0]>=BOARD_H || p[0]<0 || p[1]>=BOARD_W || p[1]<0){
                                continue;
                            }
                            now_piece = self_board[p[0]][p[1]];
                            if(now_piece){
                                continue;
                            }

                            all_actions.push_back(Move(Point(i, j), Point(p[0], p[1])));

                            oppn_piece = oppn_board[p[0]][p[1]];
                            if(oppn_piece==6){
                                this->game_state = WIN;
                                this->legal_actions = all_actions;
                                return;
                            }
                        }
                        break;
                }
            }
        }
    }
    this->legal_actions = all_actions;
}


/*============================================================
 * Bitboard move generation
 *
 * 6x5 = 30 squares fit in a uint32_t.
 * Square (r,c) -> bit index r*5+c.
 * Precomputed attack masks for leapers (knight, king, pawn).
 * Bit-scan loop (__builtin_ctz) replaces nested array iteration.
 *============================================================*/
#define BB_SQ(r, c)  ((r) * BOARD_W + (c))
#define BB_ROW(sq)   ((sq) / BOARD_W)
#define BB_COL(sq)   ((sq) % BOARD_W)

// Precomputed attack tables (initialized once)
static uint32_t bb_knight[30];       // knight attack mask per square
static uint32_t bb_king[30];         // king attack mask per square
static uint32_t bb_pawn_push[2][30]; // pawn push target per player/square
static uint32_t bb_pawn_cap[2][30];  // pawn capture targets per player/square
static bool bb_ready = false;

// Sliding piece direction vectors (0-3: rook, 4-7: bishop, 0-7: queen)
static const int bb_dr[8] = {0, 0, 1, -1, 1, 1, -1, -1};
static const int bb_dc[8] = {1, -1, 0, 0, 1, -1, 1, -1};

static void bb_init(){
    static const int kn_dr[8] = {1, 1, -1, -1, 2, 2, -2, -2};
    static const int kn_dc[8] = {2, -2, 2, -2, 1, -1, 1, -1};
    static const int ki_dr[8] = {1, 0, -1, 0, 1, 1, -1, -1};
    static const int ki_dc[8] = {0, 1, 0, -1, 1, -1, 1, -1};

    for(int r = 0; r < BOARD_H; r++){
        for(int c = 0; c < BOARD_W; c++){
            int sq = BB_SQ(r, c);

            // Knight
            bb_knight[sq] = 0;
            for(int d = 0; d < 8; d++){
                int nr = r + kn_dr[d], nc = c + kn_dc[d];
                if(nr >= 0 && nr < BOARD_H && nc >= 0 && nc < BOARD_W){
                    bb_knight[sq] |= 1u << BB_SQ(nr, nc);
                }
            }

            // King
            bb_king[sq] = 0;
            for(int d = 0; d < 8; d++){
                int nr = r + ki_dr[d], nc = c + ki_dc[d];
                if(nr >= 0 && nr < BOARD_H && nc >= 0 && nc < BOARD_W){
                    bb_king[sq] |= 1u << BB_SQ(nr, nc);
                }
            }

            // Pawn (player 0 = white, advances up = row-1)
            bb_pawn_push[0][sq] = 0;
            bb_pawn_cap[0][sq] = 0;
            if(r > 0){
                bb_pawn_push[0][sq] = 1u << BB_SQ(r-1, c);
                if(c > 0){
                    bb_pawn_cap[0][sq] |= 1u << BB_SQ(r-1, c-1);
                }
                if(c < BOARD_W-1){
                    bb_pawn_cap[0][sq] |= 1u << BB_SQ(r-1, c+1);
                }
            }

            // Pawn (player 1 = black, advances down = row+1)
            bb_pawn_push[1][sq] = 0;
            bb_pawn_cap[1][sq] = 0;
            if(r < BOARD_H-1){
                bb_pawn_push[1][sq] = 1u << BB_SQ(r+1, c);
                if(c > 0){
                    bb_pawn_cap[1][sq] |= 1u << BB_SQ(r+1, c-1);
                }
                if(c < BOARD_W-1){
                    bb_pawn_cap[1][sq] |= 1u << BB_SQ(r+1, c+1);
                }
            }
        }
    }
    bb_ready = true;
}

void State::get_legal_actions_bitboard(){
    if(!bb_ready){
        bb_init();
    }

    this->game_state = NONE;
    this->legal_actions.clear();
    this->legal_actions.reserve(64);

    int self = this->player;
    int oppn = 1 - self;

    // Build occupancy bitmasks and piece-type lookup
    uint32_t self_occ = 0, oppn_occ = 0;
    int self_pt[30] = {};  // piece type at each square (self)
    int oppn_pt[30] = {};  // piece type at each square (opponent)

    for(int r = 0; r < BOARD_H; r++){
        for(int c = 0; c < BOARD_W; c++){
            int sq = BB_SQ(r, c);
            if(this->board.board[self][r][c]){
                self_occ |= 1u << sq;
                self_pt[sq] = this->board.board[self][r][c];
            }
            if(this->board.board[oppn][r][c]){
                oppn_occ |= 1u << sq;
                oppn_pt[sq] = this->board.board[oppn][r][c];
            }
        }
    }

    uint32_t all_occ = self_occ | oppn_occ;

    // Iterate own pieces via bit scan
    uint32_t pieces = self_occ;
    while(pieces){
        int sq = __builtin_ctz(pieces);
        pieces &= pieces - 1;
        int r = BB_ROW(sq), c = BB_COL(sq);
        int piece = self_pt[sq];
        uint32_t targets = 0;

        switch(piece){
            case 1: { // Pawn
                uint32_t push = bb_pawn_push[self][sq] & ~all_occ;
                uint32_t cap = bb_pawn_cap[self][sq] & oppn_occ;
                // Check for king capture in captures
                uint32_t cap_scan = cap;
                while(cap_scan){
                    int to = __builtin_ctz(cap_scan);
                    cap_scan &= cap_scan - 1;
                    if(oppn_pt[to] == 6){
                        this->game_state = WIN;
                        this->legal_actions.push_back(
                            Move(Point(r, c), Point(BB_ROW(to), BB_COL(to))));
                        return;
                    }
                }
                targets = push | cap;
                break;
            }

            // 騎士攻擊遮罩（以位棋表示法處理）：8 個 L 形目標
            case 3: { // Knight
                targets = bb_knight[sq] & ~self_occ;
                uint32_t opp_targets = targets & oppn_occ;
                while(opp_targets){
                    int to = __builtin_ctz(opp_targets);
                    opp_targets &= opp_targets - 1;
                    if(oppn_pt[to] == 6){
                        this->game_state = WIN;
                        this->legal_actions.push_back(
                            Move(Point(r, c), Point(BB_ROW(to), BB_COL(to))));
                        return;
                    }
                }
                break;
            }

            case 6: { // King
                targets = bb_king[sq] & ~self_occ;
                uint32_t opp_targets = targets & oppn_occ;
                while(opp_targets){
                    int to = __builtin_ctz(opp_targets);
                    opp_targets &= opp_targets - 1;
                    if(oppn_pt[to] == 6){
                        this->game_state = WIN;
                        this->legal_actions.push_back(
                            Move(Point(r, c), Point(BB_ROW(to), BB_COL(to))));
                        return;
                    }
                }
                break;
            }

            case 2: // Rook
            case 4: // Bishop
            case 5: { // Queen
                int d_start = (piece == 4) ? 4 : 0;
                int d_end   = (piece == 2) ? 4 : 8;
                for(int d = d_start; d < d_end; d++){
                    int cr = r + bb_dr[d], cc = c + bb_dc[d];
                    while(cr >= 0 && cr < BOARD_H && cc >= 0 && cc < BOARD_W){
                        int to = BB_SQ(cr, cc);
                        uint32_t to_bit = 1u << to;
                        if(self_occ & to_bit){
                            break; // own piece blocks
                        }

                        if((oppn_occ & to_bit) && oppn_pt[to] == 6){
                            this->game_state = WIN;
                            this->legal_actions.push_back(
                                Move(Point(r, c), Point(cr, cc)));
                            return;
                        }

                        targets |= to_bit;
                        if(oppn_occ & to_bit){
                            break; // captured, stop sliding
                        }
                        cr += bb_dr[d]; cc += bb_dc[d];
                    }
                }
                break;
            }
        }

        // Convert target bitmask to Move objects
        while(targets){
            int to = __builtin_ctz(targets);
            targets &= targets - 1;
            this->legal_actions.push_back(
                Move(Point(r, c), Point(BB_ROW(to), BB_COL(to))));
        }
    }
}


/*============================================================
 * Dispatcher
 *============================================================*/
void State::get_legal_actions(){
    #ifdef USE_BITBOARD
    get_legal_actions_bitboard();
    #else
    get_legal_actions_naive();
    #endif
}


const char piece_table[2][7][5] = {
  {" ", "♙", "♖", "♘", "♗", "♕", "♔"},
  {" ", "♟", "♜", "♞", "♝", "♛", "♚"}
};
/**
 * @brief encode the output for command line output
 * 
 * @return std::string 
 */
std::string State::encode_output() const{
    std::stringstream ss;
    int now_piece;
    for(int i=0; i<BOARD_H; i+=1){
        for(int j=0; j<BOARD_W; j+=1){
            if((now_piece = this->board.board[0][i][j])){
                ss << std::string(piece_table[0][now_piece]);
            }else if((now_piece = this->board.board[1][i][j])){
                ss << std::string(piece_table[1][now_piece]);
            }else{
                ss << " ";
            }
            ss << " ";
        }
        ss << "\n";
    }
    return ss.str();
}


/**
 * @brief encode the state to the format for player
 * 
 * @return std::string 
 */
std::string State::encode_state(){
    std::stringstream ss;
    ss << this->player;
    ss << "\n";
    for(int pl=0; pl<2; pl+=1){
        for(int i=0; i<BOARD_H; i+=1){
            for(int j=0; j<BOARD_W; j+=1){
                ss << int(this->board.board[pl][i][j]);
                ss << " ";
            }
            ss << "\n";
        }
        ss << "\n";
    }
    return ss.str();
}


BaseState* State::create_null_state() const{
    State* s = new State(this->board, 1 - this->player);
    s->get_legal_actions();
    return s;
}


/* === Board serialization === */
static const char* piece_chars = ".PRNBQK";
static const char* piece_chars_lower = ".prnbqk";

std::string State::encode_board() const{
    std::string s;
    for(int r = 0; r < BOARD_H; r++){
        if(r > 0){
            s += '/';
        }
        for(int c = 0; c < BOARD_W; c++){
            int w = board.board[0][r][c];
            int b = board.board[1][r][c];
            if(w > 0 && w <= 6){
                s += piece_chars[w];
            }else if(b > 0 && b <= 6){
                s += piece_chars_lower[b];
            }else{
                s += '.';
            }
        }
    }
    return s;
}

void State::decode_board(const std::string& s, int side_to_move){
    player = side_to_move;
    game_state = UNKNOWN;
    zobrist_valid = false;
    board = Board{};
    int r = 0, c = 0;
    for(char ch : s){
        if(ch == '/'){
            r++;
            c = 0;
            continue;
        }
        if(r >= BOARD_H || c >= BOARD_W){
            break;
        }
        if(ch >= 'A' && ch <= 'Z'){
            for(int p = 1; p <= 6; p++){
                if(piece_chars[p] == ch){
                    board.board[0][r][c] = p;
                    break;
                }
            }
        }else if(ch >= 'a' && ch <= 'z'){
            for(int p = 1; p <= 6; p++){
                if(piece_chars_lower[p] == ch){
                    board.board[1][r][c] = p;
                    break;
                }
            }
        }
        c++;
    }
    get_legal_actions();
}


/* (Zobrist tables moved above next_state) */


/*============================================================
 * Cell display for protocol (d command)
 *============================================================*/
std::string State::cell_display(int row, int col) const{
    int w = static_cast<int>(board.board[0][row][col]);
    int b = static_cast<int>(board.board[1][row][col]);
    if(w){
        const char* names = ".PRNBQK";
        return std::string(" ") + names[w] + " ";
    }else if(b){
        const char* names = ".prnbqk";
        return std::string(" ") + names[b] + " ";
    }else{
        return " . ";
    }
}

/* === Repetition: chess 3-fold rule === */
bool State::check_repetition(const GameHistory& history, int& out_score) const {
    //規則 修改：對齊評分用的 Python 版規則——和棋是「4 次重複」(minichess_engine.py
    // 的 hash_counts>=4)，原本用 3 次會讓引擎太早把局面當和棋（分數 0）而放棄爭勝。
    if(history.count(hash()) >= 4){
        out_score = 0;  /* draw */
        return true;
    }
    return false;
}
