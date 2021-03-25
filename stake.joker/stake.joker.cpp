#include <eosio/eosio.hpp>
#include <eosio/asset.hpp>
#include <eosio/symbol.hpp>
#include <eosio/system.hpp>
#include <eosio/action.hpp>
#include <eosio/singleton.hpp>
using namespace eosio;
using namespace std;

class [[eosio::contract("stake.joker")]] prejoker : public contract {
public:
    prejoker(name receiver, name code, datastream<const char*>ds) : contract(receiver, code, ds){}
    [[eosio::on_notify("eosio.token::transfer")]]
    void on_eos_transfer(name from, name to, asset quantity, std::string memo) {
        if (from == _self || to != _self || is_gov(from)) {
            return;
        }
        check(memo == "staking", "invalid memo");
        memo = memo + ":" + from.to_string();
        action(permission_level{_self, "active"_n},
            "eosio.token"_n,
            "transfer"_n,
            make_tuple(_self, POST_CONTRACT, quantity, memo)
        ).send();
    }

private:
    const name POST_CONTRACT = "pool.joker"_n;
    bool is_gov(const name account) {
        if (account == "eosio"_n) {
            return true;
        }
        string prefix{"eosio."};
        string a = account.to_string();
        auto match = mismatch(prefix.begin(), prefix.end(), a.begin(), a.end());
        return match.first == prefix.end();
    }
};