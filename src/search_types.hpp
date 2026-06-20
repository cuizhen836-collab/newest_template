#pragma once
#include "base_state.hpp"
#include "search_params.hpp"
#include <vector>
#include <cstdint>
#include <functional>
#include <chrono>   //時控 deadline 需要 steady_clock 時間型別

class State;

struct RootUpdate {
    Move best_move;
    int score;
    int depth;
    int move_number;
    int total_moves;
};

struct SearchContext {
    uint64_t nodes = 0;
    int seldepth = 0;
    bool stop = false;
    ParamMap params;
    std::function<void(const RootUpdate&)> on_root_update;

    //時控 
    // 時間控制：當 has_deadline 為 true 時，搜尋一旦超過 deadline 就會
    // 自己把 stop 設成 true 中止。deadline 故意不在 reset() 裡清除，
    // 這樣同一步棋裡「逐層加深」的每一層都會沿用同一個截止時間。
    bool has_deadline = false;                       //時控 是否啟用截止時間
    std::chrono::steady_clock::time_point deadline{};//時控 本步搜尋的截止時刻
    //時控 

    //穩定
    // root 優先搜尋的提示走法（通常是上一輪迭代加深的最佳走法）。
    // 讓搜尋被時間中斷時，手上的「目前最佳走法」是重新驗證過的好棋，
    // 而不是 MVV-LVA 剛好排在最前的隨機吃子。reset() 不清除，由 do_search 每層設定。
    bool has_root_hint = false;
    Move root_hint{};
    //穩定 

    void reset(){
        nodes = 0;
        seldepth = 0;
    }
};

struct SearchResult {
    Move best_move;
    int score = 0;
    int depth = 0;
    int seldepth = 0;
    uint64_t nodes = 0;
    double time_ms = 0;
    std::vector<Move> pv;
};
