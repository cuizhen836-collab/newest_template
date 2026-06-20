#pragma once
#include "search_types.hpp"
#include "game_history.hpp"

struct MMParams {
    bool use_kp_eval = true;
    bool use_eval_mobility = true;
    bool report_partial = true;
    int  contempt = 0;   //反重複 軟性重複罰分（內部分數尺度，pawn≈20）。0=關閉。
    bool use_pvs = true; //PVS 是否啟用 Principal Variation Search（零視窗偵察+重搜）。

    static MMParams from_map(const ParamMap& m){
        MMParams p;
        p.use_kp_eval       = param_bool(m, "UseKPEval", true);
        p.use_eval_mobility = param_bool(m, "UseEvalMobility", true);
        p.report_partial    = param_bool(m, "ReportPartial", true);
        p.contempt          = param_int(m, "Contempt", 0);
        p.use_pvs           = param_bool(m, "UsePVS", true);
        return p;
    }
};

class MiniMax{
public:
    static int eval_ctx(
        State *state,
        int depth,
        GameHistory& history,
        int ply,
        int alpha,
        int beta,
        SearchContext& ctx,
        const MMParams& p
    );
    static SearchResult search(
        State *state,
        int depth,
        GameHistory& history,
        SearchContext& ctx
    );

    static ParamMap default_params();
    static std::vector<ParamDef> param_defs();
};
