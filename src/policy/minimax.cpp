#include <utility>
#include <chrono>   
#include <algorithm>
#include <vector>  
#include "state.hpp"
#include "minimax.hpp"


//排序 
// MVV-LVA 走法排序：把「吃子」排在「靜步」前面，並讓「吃大子、用小子吃」
// 的走法優先。alpha-beta 先搜到好棋時，cutoff 會更早發生，剪枝更猛、
// 同樣時間能搜更深。排序不會改變搜尋結果，只影響效率與同分時的取捨。
// 一個走法 m = ((from_row,from_col),(to_row,to_col))；
// 若對手在目的格有子，就是吃子，victim 為被吃子、aggressor 為己方出動的子。
static int move_order_score(State* state, const Move& m){
    int to_r = (int)m.second.first;
    int to_c = (int)m.second.second;
    int victim = state->piece_at(1 - state->player, to_r, to_c);
    if(victim == 0){
        return 0;                       // 靜步（沒吃到子）
    }
    int from_r = (int)m.first.first;
    int from_c = (int)m.first.second;
    int aggressor = state->piece_at(state->player, from_r, from_c);
    // 吃子一律 +10000 排到最前；victim 越大、aggressor 越小，分數越高。
    return 10000 + PIECE_VALUES[victim] * 100 - PIECE_VALUES[aggressor];
}

// 把 state->legal_actions 依 MVV-LVA 分數由高到低就地排序。
// 分數先算好存進可重複使用的緩衝區，避免在比較器裡重算、也避免每個節點都配置記憶體。
static void order_moves(State* state){
    static thread_local std::vector<std::pair<int, Move>> scored;
    auto& acts = state->legal_actions;
    scored.clear();
    for(auto& m : acts){
        scored.emplace_back(move_order_score(state, m), m);
    }
    std::sort(scored.begin(), scored.end(),
        [](const std::pair<int, Move>& a, const std::pair<int, Move>& b){
            return a.first > b.first;
        });
    for(size_t i = 0; i < acts.size(); i++){
        acts[i] = scored[i].second;
    }
}



//靜止 
// Quiescence Search（靜止搜尋）
// 主搜尋到 depth<=0 時，不直接用靜態評估打分，而是「只繼續延伸吃子」直到盤面安定，
// 避免在吃子序列進行到一半就打分（水平線效應 horizon effect）——這正是先前
// 「執黑看到 0.00、下一手突然被將死」的根因。
//   stand-pat：側方可選擇不吃子，靜態評估當作分數下界。
//   之後只展開吃子走法（MVV-LVA 排序）做 alpha-beta，直到沒有吃子可走。
// 吃子會永久減少棋子，序列必然有限，不會無限遞迴。
static int quiescence(
    State* state,
    GameHistory& history,
    int ply,
    int alpha,
    int beta,
    SearchContext& ctx,
    const MMParams& p
){
    ctx.nodes++;
    if(ply > ctx.seldepth){ ctx.seldepth = ply; }
    // 時控：與 eval_ctx 相同的截止時間節流檢查
    if(ctx.has_deadline && (ctx.nodes & 2047) == 0
        && std::chrono::steady_clock::now() >= ctx.deadline){
        ctx.stop = true;
    }
    if(ctx.stop){ return 0; }

    if(state->legal_actions.empty() && state->game_state == UNKNOWN){
        state->get_legal_actions();
    }
    // 終局處理與 eval_ctx 一致
    if(state->game_state == WIN){ return P_MAX - ply; }
    if(state->game_state == DRAW){ return 0; }

    // stand-pat：不吃子時的靜態評估，當作目前能保證的下界
    int stand_pat = state->evaluate(p.use_kp_eval, p.use_eval_mobility, &history);
    if(stand_pat >= beta){ return stand_pat; }   // fail-high：對手不會讓我們走到這
    if(stand_pat > alpha){ alpha = stand_pat; }
    int best_score = stand_pat;

    // 收集吃子走法（目的格有對方棋子），用 MVV-LVA 排序（先吃大子、用小子吃）
    std::vector<std::pair<int, Move>> caps;
    caps.reserve(state->legal_actions.size());
    for(auto& m : state->legal_actions){
        int victim = state->piece_at(1 - state->player,
                                     (int)m.second.first, (int)m.second.second);
        if(victim == 0){ continue; }   // 非吃子，靜止搜尋略過
        int aggressor = state->piece_at(state->player,
                                        (int)m.first.first, (int)m.first.second);
        caps.emplace_back(PIECE_VALUES[victim] * 100 - PIECE_VALUES[aggressor], m);
    }
    std::sort(caps.begin(), caps.end(),
        [](const std::pair<int, Move>& a, const std::pair<int, Move>& b){
            return a.first > b.first;
        });

    for(auto& entry : caps){
        State* next = state->next_state(entry.second);
        bool same = next->same_player_as_parent();
        int child_alpha = same ? alpha : -beta;
        int child_beta  = same ? beta  : -alpha;
        int raw = quiescence(next, history, ply + 1, child_alpha, child_beta, ctx, p);
        int score = same ? raw : -raw;
        // mate 距離調整，與 eval_ctx 一致
        if(score > P_MAX / 2){ score -= 1; }
        else if(score < -P_MAX / 2){ score += 1; }
        delete next;

        if(score > best_score){
            best_score = score;
            if(score > alpha){ alpha = score; }
        }
        if(alpha >= beta){ break; }   // beta cutoff
        if(ctx.stop){ break; }
    }
    return best_score;
}


/*============================================================
 * MiniMax — eval_ctx
 *
 * Negamax with alpha-beta pruning + MVV-LVA move ordering.
 * Caller manages memory.
 *============================================================*/
int MiniMax::eval_ctx(
    State *state,
    int depth,
    GameHistory& history,
    int ply,
    int alpha,
    int beta,
    SearchContext& ctx,
    const MMParams& p
){
    ctx.nodes++;
    if(ply > ctx.seldepth){
        ctx.seldepth = ply;
    }
    //時控 
    // 每搜約 2048 個節點才看一次時鐘（用 & 2047 節流，避免拖慢熱迴圈）。
    // 一旦超過截止時刻，就把 stop 設成 true，讓整個遞迴盡快收掉。
    if(ctx.has_deadline && (ctx.nodes & 2047) == 0
        && std::chrono::steady_clock::now() >= ctx.deadline){
        ctx.stop = true;
    }
    
    if(ctx.stop){
        return 0;
    }

    /* === Lazy move generation (sets game_state) === */
    if(state->legal_actions.empty() && state->game_state == UNKNOWN){
        state->get_legal_actions();
    }

    /* === Terminal / leaf checks === */

    // [ Hackathon TODO 3-1 ]
    // return the score for a winning terminal state
    // Hint: prefer faster wins by using ply.
    // 如果目前狀態是 WIN，代表當前行動方已獲勝，回傳 P_MAX 減去 ply 以偏好較快的勝利
    if(state->game_state == WIN){
        return P_MAX - ply;
    }  //至

    if(state->game_state == DRAW){
        return 0;
    }

    /* === Repetition check (game-specific) === */
    int rep_score;
    if(state->check_repetition(history, rep_score)){
        return rep_score;
    }
    //反重複 
    // 軟性重複 contempt：若此局面在（實際對局史 + 目前搜尋路徑）已出現過至少兩次，
    // 視為「很可能走向和棋」，回傳 -contempt（站在 side-to-move 視角）。
    //   領先方：-contempt < 它真正的正分 → 避開重複、改找有進展的走法（破解原地擺子）。
    //   落後方：-contempt 仍遠高於它真正的負分 → 樂意導向和棋（行為正確）。
    // 只在 Contempt!=0 時啟用；預設 0=關閉，完全不改原行為。短逼著的 strong 對局
    // 分數差很大、不會走到重複，故此項對 strong 零影響。
    if(p.contempt != 0 && history.count(state->hash()) >= 2){
        return -p.contempt;
    }
    //反重複 
    history.push(state->hash());

    if(depth <= 0){
        //靜止 修改：葉節點改用 quiescence search（只延伸吃子）取代直接靜態評估，
        // 把吃子序列搜到安定為止，消除水平線效應。主 negamax 不變。
        int score = quiescence(state, history, ply, alpha, beta, ctx, p);
        history.pop(state->hash());
        return score;
    }

    //排序 展開子節點前先做 MVV-LVA 排序，讓 cutoff 更早發生
    order_moves(state);

    /* === Negamax loop（alpha-beta，可選 PVS / NegaScout）=== */
    // PVS 新增：第一手（經 MVV-LVA 排序的最佳手）用全視窗精確搜；其餘走法先用
    // 零視窗 (alpha, alpha+1) 偵察，假設它們不如 PV——多半很快 fail-low 被剪掉。
    // 只有偵察 fail-high（可能更好）且落在 (alpha,beta) 內時，才用全視窗重搜成精確值。
    // 全視窗下（root 對每個走法即如此）PVS 必定重搜出精確值，故與純 alpha-beta
    // 在固定深度回傳完全相同的分數與選棋；差別只在更早剪枝、更快、同時間搜更深。
    int best_score = M_MAX;
    bool first = true;

    for(auto& action : state->legal_actions){
        State* next = state->next_state(action);

        bool same = next->same_player_as_parent();

        // Alpha-Beta：換對手時把視窗翻轉成 [-beta, -alpha]，同一方再走則沿用。
        int raw_score;
        if(!p.use_pvs || first){
            // 全視窗（PV 走法，或關閉 PVS 時退化成原本的 alpha-beta）
            int ca = same ? alpha : -beta;
            int cb = same ? beta  : -alpha;
            raw_score = eval_ctx(next, depth - 1, history, ply + 1, ca, cb, ctx, p);
        }else{
            // 零視窗偵察
            int ca = same ? alpha       : -(alpha + 1);
            int cb = same ? (alpha + 1) : -alpha;
            raw_score = eval_ctx(next, depth - 1, history, ply + 1, ca, cb, ctx, p);
            int probe = same ? raw_score : -raw_score;
            // fail-high 且在視窗內 → 全視窗重搜成精確值（時間已到則不重搜）
            if(probe > alpha && probe < beta && !ctx.stop){
                int fa = same ? alpha : -beta;
                int fb = same ? beta  : -alpha;
                raw_score = eval_ctx(next, depth - 1, history, ply + 1, fa, fb, ctx, p);
            }
        }
        int score = same ? raw_score : -raw_score;

        // 對將死/被將死分數做距離調整（mate distance），向上傳播時減少/增加 1 ply。
        if(score > P_MAX / 2){
            score = score - 1;
        }else if(score < -P_MAX / 2){
            score = score + 1;
        }

        delete next;

        // update best_score if this child is better.
        if(score > best_score){
            best_score = score;
            // 只在分數真的超過目前 alpha 時才提高 alpha（取 max）。
            if(score > alpha){
                alpha = score;
            }
            // 若已幾乎必勝，就break
            if(best_score >= P_MAX - ply){
                break;
            }
        }

        // Alpha-Beta 剪枝：alpha >= beta 代表後面的子節點不影響結果，直接剪掉。
        if(alpha >= beta){
            break;
        }

        first = false;   //PVS 新增：之後的走法改用零視窗偵察
    }

    history.pop(state->hash());
    return best_score;
}


/*============================================================
 * MiniMax — search
 *
 * Iterate legal moves, call eval_ctx, return SearchResult.
 *============================================================*/
SearchResult MiniMax::search(
    State *state,
    int depth,
    GameHistory& history,
    SearchContext& ctx
){
    ctx.reset();
    MMParams p = MMParams::from_map(ctx.params);
    SearchResult result;
    result.depth = depth;

    if(!state->legal_actions.size()){
        state->get_legal_actions();
    }

    //排序 root 也排序，先搜可能的好棋（吃子優先）
    order_moves(state);

    //穩定 新增：把上一輪的最佳走法排到最前面（PV move first）。
    // 被時間中斷時，目前最佳走法就是「重新驗證過的好棋」而非隨機吃子；
    // 先搜好棋也讓 root 的 alpha-beta 剪枝更早觸發。
    if(ctx.has_root_hint){
        auto& acts = state->legal_actions;
        auto it = std::find(acts.begin(), acts.end(), ctx.root_hint);
        if(it != acts.end()){
            std::rotate(acts.begin(), it, it + 1);  // 移到最前，其餘相對順序不變
        }
    }

    int best_score = M_MAX - 10;
    //PVS root 也做 alpha-beta + PVS。alpha 從 -inf 起、隨找到的最佳分數提高；
    // beta 固定 +inf（root 沒有上層可剪枝，必須看完每個走法以選出最佳手）。
    // 第一手用全視窗求精確值；其餘走法先用零視窗偵察，只有「可能更好」才用全視窗重搜。
    // 選棋判斷 (score > best_score) 與原本完全相同 → 選的棋、回報的 best_score 不變，
    // 只是非最佳手不再花全視窗搜尋 → 節點大降、同時間搜更深。
    int alpha = M_MAX;
    const int beta = P_MAX;
    int move_index = 0;
    int total_moves = (int)state->legal_actions.size();
    bool first = true;

    for(auto& action : state->legal_actions){
        State* next = state->next_state(action);
        // 把「目前棋盤」記錄進歷史紀錄
        history.push(state->hash());

        bool same = next->same_player_as_parent();//下一個state是？

        int raw_score;
        if(!p.use_pvs || first){
            // 第一手（或關閉 PVS）：全視窗精確搜尋
            int ca = same ? alpha : -beta;
            int cb = same ? beta  : -alpha;
            raw_score = eval_ctx(next, depth - 1, history, 1, ca, cb, ctx, p);
        }else{
            // 零視窗偵察
            int ca = same ? alpha       : -(alpha + 1);
            int cb = same ? (alpha + 1) : -alpha;
            raw_score = eval_ctx(next, depth - 1, history, 1, ca, cb, ctx, p);
            int probe = same ? raw_score : -raw_score;
            // fail-high（可能比目前最佳手好）→ 全視窗重搜成精確值（時間到則不重搜）
            if(probe > alpha && !ctx.stop){
                int fa = same ? alpha : -beta;
                int fb = same ? beta  : -alpha;
                raw_score = eval_ctx(next, depth - 1, history, 1, fa, fb, ctx, p);
            }
        }
        int score = same ? raw_score : -raw_score;

        // pop the parent hash we pushed above
        history.pop(state->hash());
        delete next;//至

            if(score > best_score){
                // keep this move if it is the best so far
                best_score = score;
                if(score > alpha){ alpha = score; }   //PVS 新增：提高 root alpha 收緊偵察視窗
                result.best_move = action;
                result.score = best_score;
                //如果「允許回報」而且「有回報功能」，就把目前 AI 的搜尋進度傳出去。
                if(p.report_partial && ctx.on_root_update){
                   ctx.on_root_update({result.best_move, best_score, depth, move_index + 1, total_moves});
                }
            }
        first = false;   //PVS 
        move_index++;
    }

    // [ Hackathon TODO 4-3 ]
    // update result and return
    //return result
    // 將搜尋統計填入 `result`（深度、seldepth、節點數），並回傳最終結果
    result.depth = depth;
    result.seldepth = ctx.seldepth;//實際搜尋最深層
    result.nodes = ctx.nodes;
    return result;
} 


/*============================================================
 * MiniMax — default_params / param_defs
 *============================================================*/
ParamMap MiniMax::default_params(){
    return {
        {"UseKPEval", "true"},
        {"UseEvalMobility", "true"},
        {"ReportPartial", "true"},
        {"Contempt", "15"},  //反重複 預設啟用，防止「領先時原地擺子→把優勢拖成輸」
        {"UsePVS", "true"},  //PVS 預設啟用（固定深度結果不變、時間制更深）
    };
}

std::vector<ParamDef> MiniMax::param_defs(){
    return {
        {"UseKPEval", ParamDef::CHECK, "true"},
        {"UseEvalMobility", ParamDef::CHECK, "true"},
        {"ReportPartial", ParamDef::CHECK, "true"},
        {"Contempt", ParamDef::SPIN, "15", 0, 100},  //反重複 修0=關；預設 15
        {"UsePVS", ParamDef::CHECK, "true"},         //PVS 
    };
}

