#include <eosio/eosio.hpp>
#include <eosio/asset.hpp>
#include <eosio/symbol.hpp>
#include <eosio/system.hpp>
#include <eosio/action.hpp>
#include <eosio/singleton.hpp>
using namespace eosio;
using namespace std;

class [[eosio::contract]] joker : public contract {
public:
    joker(name receiver, name code, datastream<const char*>ds) : contract(receiver, code, ds), state_table(receiver, receiver.value) {
        if (!state_table.exists()) {
            uint32_t day = sec_to_day(current_time_point().sec_since_epoch());
            state_table.get_or_create(_self, state{day, asset(0, EOS_SYM), INIT_DAY_REWARDS, asset(0, TOKEN_SYM), 0, false});
        }
    }
    [[eosio::on_notify("eosio.token::transfer")]]
    void on_eos_transfer(name from, name to, asset quantity, std::string memo) {
        if (from == _self || to != _self || is_gov(from)) {
            return;
        }
        if (from != PRE_CONTRACT) {
            transfer("eosio.token"_n, FEE_ACCOUNT, quantity, memo);
            return;
        }
        auto s = state_table.get();
        check(s.started, "the pool has not started");
        string prefix{"staking:"};
        auto match = mismatch(prefix.begin(), prefix.end(), memo.begin());
        check(match.first == prefix.end(), "invalid memo");
        name user(memo.substr(prefix.size()));
        check(quantity.symbol == EOS_SYM, "we only need eos");
        check(quantity.is_valid(), "invalid quantity" );
        check(quantity.amount >= EOS_MIN_TRANSFER, "too little quantity");
        check(memo.size() > prefix.size() && memo.size() <= 256, "memo has more than 256 bytes" );

        asset fee = quantity * EOS_FEE_RATE_1000 / 1000;
        asset staking = quantity - fee;

        update_state(staking, _self);

        pool_index pools(_self, user.value);
        uint32_t sec = current_time_point().sec_since_epoch();
        uint32_t day = sec_to_day(sec);
        check(day > 0, "error");
        asset buyrexresult = to_rex(staking);
        pools.emplace(_self, [&](auto& row) {
            row.key = pools.available_primary_key();
            row.created = sec;
            row.staking = staking;
            row.mining_pool = staking;
            row.total_reward = asset(0, TOKEN_SYM);
            row.total_rex = buyrexresult;
            row.last_reward_day = day - 1;
        });
        transfer("eosio.token"_n, FEE_ACCOUNT, fee, string("staking fee"));
    }

    [[eosio::on_notify("joker.eos::transfer")]]
    void on_token_transfer(name from, name to, asset quantity, std::string memo) {
        if (from == _self || to != _self || is_gov(from)) {
            return;
        }
        if (memo == "adminop") {
            return;
        }
        string prefix{"order:"};
        check(quantity.symbol == TOKEN_SYM, "we only need joker");
        check(quantity.is_valid(), "invalid quantity");
        check(quantity.amount > 0, "invalid quantity");
        check(memo.size() > prefix.size() && memo.size() <= 256, "invalid memo");

        auto match = mismatch(prefix.begin(), prefix.end(), memo.begin());
        check(match.first == prefix.end(), "invalid memo");
        memo = memo.substr(prefix.size());
        int64_t order = -1;
        read_order(memo, &order);
        check(order >= 0, "invalid memo");
        pool_index pools(_self, from.value);
        auto iter = pools.find(order);
        check(iter != pools.end(), "order not found");
        const uint32_t r = iter->created % 86400;
        const uint32_t redeem_time = iter->created - r + 5 * 86400;
        bool redeemable = current_time_point().sec_since_epoch() > redeem_time;
        check(redeemable, "order is still frozen");
        asset fee = iter->total_reward * TOKEN_FEE_RATE_1000 / 1000;
        asset need = iter->total_reward + fee;
        check(quantity >= need, "token not enough");
        asset change = quantity - need;
        asset back = iter->staking;
        check(back.amount > 0, "staking is zero");
        asset total_rex = iter->total_rex;
        check(total_rex.amount > 0, "total rex is zero");

        update_state(asset(0, EOS_SYM), _self);
        asset mining_pool = iter->mining_pool;
        asset reward{0, TOKEN_SYM};
        uint32_t last_reward_day = iter->last_reward_day;
        asset calc_mining = update_user_pool(iter, reward, mining_pool, last_reward_day);

        auto s = state_table.get();
        if (s.total_reward.amount >= (iter->total_reward + reward).amount) {
            s.total_reward -= iter->total_reward + reward;
        } else {
            s.total_reward.amount = 0;
        }
        update_reward_per_day(s.total_reward, s.reward_per_day, s.round);
        state_table.set(s, _self);

        update_state(-calc_mining, _self);

        pools.erase(iter);
        asset eosfund = from_rex(total_rex);
        check(eosfund.amount >= back.amount, "rex income error");
        transfer("eosio.token"_n, from, back, string("redeem"));
        transfer("eosio.token"_n, FEE_ACCOUNT, eosfund-back, string("rex income"));
        transfer(TOKEN_CONTRACT, FEE_ACCOUNT, fee, string("redeem fee"));
        transfer(TOKEN_CONTRACT, from, change, string("redeem change"));
    }

    [[eosio::action]]
    void redeem(name user, uint64_t key) {
        require_auth(user);
        pool_index pools(_self, user.value);
        auto iter = pools.find(key);
        check(iter != pools.end(), "order not found");
        const uint32_t r = iter->created % 86400;
        const uint32_t redeem_time = iter->created - r + 5 * 86400;
        bool redeemable = current_time_point().sec_since_epoch() > redeem_time;
        check(redeemable, "order is still frozen");
        check(iter->total_reward.amount == 0, "please return reward");
        asset back = iter->staking;
        check(back.amount > 0, "staking is zero");
        asset total_rex = iter->total_rex;
        check(total_rex.amount > 0, "total rex is zero");

        update_state(asset(0, EOS_SYM), _self);

        asset mining_pool = iter->mining_pool;
        asset reward{0, TOKEN_SYM};
        uint32_t last_reward_day = iter->last_reward_day;
        asset calc_mining = update_user_pool(iter, reward, mining_pool, last_reward_day);

        auto s = state_table.get();
        if (s.total_reward.amount >= reward.amount) {
            s.total_reward -= reward;
        } else {
            s.total_reward.amount = 0;
        }
        update_reward_per_day(s.total_reward, s.reward_per_day, s.round);

        update_state(-calc_mining, _self);

        pools.erase(iter);
        asset eosfund = from_rex(total_rex);
        check(eosfund.amount >= back.amount, "rex income error");
        transfer("eosio.token"_n, user, back, string("redeem"));
        transfer("eosio.token"_n, FEE_ACCOUNT, eosfund-back, string("rex income"));
    }

    [[eosio::action]]
    void harvest(name user, uint64_t key) {
        require_auth(user);
        update_state(asset(0, EOS_SYM), user);
        pool_index pools(_self, user.value);
        auto iter = pools.find(key);
        if (iter == pools.end()) {
            return;
        }

        asset reward{0, TOKEN_SYM};
        asset mining_pool = iter->mining_pool;
        uint32_t last_reward_day = iter->last_reward_day;
        update_user_pool(iter, reward, mining_pool, last_reward_day);
        pools.modify(iter, user, [&](auto& row) {
            row.last_reward_day = last_reward_day;
            row.mining_pool = mining_pool;
            row.total_reward += reward;
        });
        transfer(TOKEN_CONTRACT, user, reward, string("reward"));
    }

    [[eosio::action]]
    void start() {
        require_auth(permission_level{_self, "active"_n});
        auto s = state_table.get();
        s.started = true;
        state_table.set(s, _self);
    }

    [[eosio::action]]
    void stop() {
        require_auth(permission_level{_self, "active"_n});
        auto s = state_table.get();
        s.started = false;
        state_table.set(s, _self);
    }

private:
    const symbol EOS_SYM{symbol_code("EOS"), 4};
    const double EOS_FEE_RATE_1000 = 5; // 0.5%
    const uint64_t EOS_MIN_TRANSFER = 1'0000;
    const symbol TOKEN_SYM{symbol_code("JOKER"), 4};
    const name TOKEN_CONTRACT = "joker.eos"_n; // !!!!IMPORTANT change notify function
    const double TOKEN_FEE_RATE_1000 = 5; // 0.5%
    const uint64_t MINING_MIN_LIMIT = 5400'0000;
    const asset INIT_DAY_REWARDS{13000'0000, TOKEN_SYM};
    const name FEE_ACCOUNT = "fee.joker"_n;
    const symbol REX_SYM{symbol_code("REX"), 4};
    const uint64_t REDUCE_AMOUNT = 520000'0000; // 520,000.0000 JOKER
    const uint64_t REDUCE_PERSENT = 12; // 12%
    const name PRE_CONTRACT = "stake.joker"_n;

    struct [[eosio::table]] state {
        uint32_t day;
        asset pool;
        asset reward_per_day;
        asset total_reward;
        uint64_t round;
        bool started;

        uint64_t primary_key() const { return 0; }
    };
    using state_index = singleton<"state"_n, state>;
    state_index state_table;

    struct [[eosio::table]] snapshot {
        uint32_t day;
        asset pool;
        double reward_per_share;

        uint64_t primary_key() const { return (uint64_t)day; }
    };
    using snapshot_index = multi_index<"snapshot"_n, snapshot>;

    struct [[eosio::table]] pool {
        uint64_t key; // order id
        uint32_t created; // timestamp in second
        // EOS
        asset staking;  // total staking eos amount
        asset mining_pool; // current mining_pool
        // TOKEN
        asset total_reward; // total received token, only inc when contract realy send token
        // REX
        asset total_rex;

        uint32_t last_reward_day;

        uint64_t primary_key() const { return key; }
    };
    using pool_index = multi_index<"pool"_n, pool>;

    struct [[eosio::table]] rexpool {
        uint8_t    version = 0;
        asset      total_lent;
        asset      total_unlent;
        asset      total_rent;
        asset      total_lendable;
        asset      total_rex;
        asset      namebid_proceeds;
        uint64_t   loan_num = 0;

        uint64_t primary_key()const { return 0; }
    };
    using rex_pool_index = multi_index<"rexpool"_n, rexpool>;

    void update_state(asset quantity, name payer) {
        uint32_t day = sec_to_day(current_time_point().sec_since_epoch());
        auto s = state_table.get();
        if (s.day < day) {
            snapshot_index snapshots(_self, _self.value);
            for ( ; s.day < day; s.day++) {
                asset weighting_pool = s.pool / 100;
                double reward_per_share = 0;
                if (weighting_pool.amount >= MINING_MIN_LIMIT) {
                    reward_per_share = (double)(s.reward_per_day * 100).amount / (double)s.pool.amount;
                    s.total_reward += s.reward_per_day;
                    update_reward_per_day(s.total_reward, s.reward_per_day, s.round);
                }
                snapshots.emplace(payer, [&](auto& row) {
                    row.day = s.day;
                    row.pool = s.pool;
                    row.reward_per_share = reward_per_share;
                });
                if (weighting_pool.amount >= MINING_MIN_LIMIT) {
                    s.pool = (s.pool * 99) / 100;
                }
            }
            if (quantity.amount != 0) {
                if (quantity.amount < 0 && s.pool.amount < -quantity.amount) {
                    s.pool.amount = 0;
                } else {
                    s.pool += quantity;
                }
            }
            state_table.set(s, _self);
        } else if (quantity.amount != 0) {
            if (quantity.amount < 0 && s.pool.amount < -quantity.amount) {
                s.pool.amount = 0;
            } else {
                s.pool += quantity;
            }
            state_table.set(s, _self);
        }
    }

    void update_reward_per_day(asset total_reward, asset &reward_per_day, uint64_t &round) {
        const int64_t epoch = (total_reward / REDUCE_AMOUNT).amount;
        if (round == epoch) {
            return;
        }
        round = epoch;
        asset reward = INIT_DAY_REWARDS;
        for (int i = 0; i < epoch; i++) {
            reward -= (reward / 100) * REDUCE_PERSENT;
        }
        reward_per_day = reward;
        return;
    }

    uint32_t sec_to_day(uint32_t sec) {
        return (sec + 3600) / 86400;
    }

    uint32_t users_day(uint32_t sec, uint32_t created) {
        return ((sec - created) / 86400) + ((created + 3600) / 86400);
    }

    void transfer(name code, name to, asset quantity, string memo) {
        if (quantity.amount > 0) {
            action(permission_level{_self, "active"_n},
                code,
                "transfer"_n,
                make_tuple(_self, to, quantity, memo)
            ).send();
        }
    }

    asset to_rex(asset quantity) {
        asset buyrexresult{0, REX_SYM};
        if (quantity.amount > 0) {
            rex_pool_index _rexpool("eosio"_n, ("eosio"_n).value);
            auto itr = _rexpool.require_find(0, "REX pool not found");
            const int64_t S0 = itr->total_lendable.amount;
            const int64_t S1 = S0 + quantity.amount;
            const int64_t R0 = itr->total_rex.amount;
            const int64_t R1 = (uint128_t(S1) * R0) / S0;
            buyrexresult.amount = R1 - R0;
            check(buyrexresult.amount > 0, "rex error");
            action(permission_level{_self, "active"_n},
                "eosio"_n,
                "deposit"_n,
                make_tuple(_self, quantity)
            ).send();
            action(permission_level{_self, "active"_n},
                "eosio"_n,
                "buyrex"_n,
                make_tuple(_self, quantity)
            ).send();
        }
        return buyrexresult;
    }

    asset from_rex(asset rex_sell) {
        asset eosfund{0, EOS_SYM};
        if (rex_sell.amount > 0) {
            rex_pool_index _rexpool("eosio"_n, ("eosio"_n).value);
            auto itr = _rexpool.require_find(0, "REX pool not found");
            const int64_t S0 = itr->total_lendable.amount;
            const int64_t R0 = itr->total_rex.amount;
            eosfund.amount = (uint128_t(rex_sell.amount) * S0) / R0;
            check(eosfund.amount > 0, "sellrex fund amount error");
            action(permission_level{_self, "active"_n},
                "eosio"_n,
                "sellrex"_n,
                make_tuple(_self, rex_sell)
            ).send();
            action(permission_level{_self, "active"_n},
                "eosio"_n,
                "withdraw"_n,
                make_tuple(_self, eosfund)
            ).send();
        }
        return eosfund;
    }

    void read_order(const string& str, int64_t* res) {
        int64_t r = 0;
        for (int i = 0; i < str.size(); i++) {
            if (str[i] < '0' || str[i] > '9') {
                return;
            }
            r = r*10 + (str[i] - '0');
        }
        *res = r;
    }

    bool is_gov(const name account) {
        if (account == "eosio"_n) {
            return true;
        }
        string prefix{"eosio."};
        string a = account.to_string();
        auto match = mismatch(prefix.begin(), prefix.end(), a.begin(), a.end());
        return match.first == prefix.end();
    }

    asset update_user_pool(const pool_index::const_iterator &iter, asset &reward, asset &mining_pool, uint32_t &last_reward_day) {
        const uint32_t uday = users_day(current_time_point().sec_since_epoch(), iter->created);
        const uint32_t day = sec_to_day(current_time_point().sec_since_epoch());
        mining_pool = iter->mining_pool;
        last_reward_day = iter->last_reward_day;
        if (iter->last_reward_day + 1 >= day) {
            return iter->mining_pool;
        }
        double reward_amount = 0;
        uint32_t reward_day = iter->last_reward_day + 1;
        asset ret{0, EOS_SYM};
        ret.amount = mining_pool.amount;
        snapshot_index snapshots(_self, _self.value);
        auto snapshot_iter = snapshots.find(reward_day);
        while(reward_day < day) {
            check(snapshot_iter != snapshots.end(), "error");
            check(snapshot_iter->day == reward_day, "error");
            if (snapshot_iter->reward_per_share > 0) {
                if (reward_day < uday) {
                    if (mining_pool.amount > 10000) {
                        reward_amount += (mining_pool.amount * snapshot_iter->reward_per_share / 100);
                    }
                    mining_pool = mining_pool * 99 / 100;
                }
                ret = ret * 99 / 100;
            }
            reward_day++;
            snapshot_iter++;
        }
        check(reward_amount < numeric_limits<int64_t>::max(), "overflow");
        reward.amount = (int64_t)reward_amount;
        last_reward_day = uday - 1;
        return ret;
    }
};