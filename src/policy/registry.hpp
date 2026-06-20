#pragma once
/*============================================================
 * Algorithm Registry
 *
 * Each algorithm defines:
 *   - search() function
 *   - default_params() returning ParamMap
 *   - param_defs() for UCI option advertisement
 *============================================================*/

#include <string>
#include <functional>
#include <vector>
#include "search_types.hpp"
#include "game_history.hpp"
#include "minimax.hpp"
#include "random.hpp"

struct AlgoEntry {
    std::string name;
    ParamMap default_params;
    std::vector<ParamDef> param_defs;
    std::function<SearchResult(State*, int, GameHistory&, SearchContext&)> search;
};

inline const std::vector<AlgoEntry>& get_algo_table(){
    static const std::vector<AlgoEntry> table = {
        {
            "minimax",
            MiniMax::default_params(),
            MiniMax::param_defs(),
            [](State* s, int d, GameHistory& h, SearchContext& c){
                return MiniMax::search(s, d, h, c);
            }
        },
        {
            // PVS 與 minimax 同一套搜尋，明確開啟 UsePVS。
            // 若以 --black-algo pvs 叫用時，引擎 find_algo("pvs") 會對應到這裡。
            "pvs",
            [](){ ParamMap p = MiniMax::default_params(); p["UsePVS"] = "true"; return p; }(),
            MiniMax::param_defs(),
            [](State* s, int d, GameHistory& h, SearchContext& c){
                return MiniMax::search(s, d, h, c);
            }
        },
        {
            "random",
            Random::default_params(),
            Random::param_defs(),
            [](State* s, int d, GameHistory& h, SearchContext& c){
                return Random::search(s, d, h, c);
            }
        },
    };
    return table;
}

inline const AlgoEntry* find_algo(const std::string& name){
    for(auto& entry : get_algo_table()){
        if(entry.name == name){
            return &entry;
        }
    }
    return nullptr;
}

inline std::string default_algo_name(){ return "minimax"; }
