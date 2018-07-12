// #include "actions_table.hpp"
#include <eosio/sql_db_plugin/actions_table.hpp>

namespace eosio {

    actions_table::actions_table(std::shared_ptr<soci::session> session):
        m_session(session) {

    }

    void actions_table::drop() {
        try {
            *m_session << "drop table IF EXISTS actions_accounts";
            *m_session << "drop table IF EXISTS actions";
        }catch(std::exception& e) {
            wlog(e.what());
        }
    }

    void actions_table::create() {
        *m_session << "CREATE TABLE `actions` ("
                        "`id` bigint(20) NOT NULL AUTO_INCREMENT,"
                        "`account` varchar(16) COLLATE utf8mb4_general_ci NOT NULL DEFAULT '',"
                        "`transaction_id` varchar(64) COLLATE utf8mb4_general_ci NOT NULL DEFAULT '',"
                        "`seq` smallint(6) NOT NULL DEFAULT 0,"
                        "`parent` bigint(20) NOT NULL DEFAULT 0,"
                        "`name` varchar(12) COLLATE utf8mb4_general_ci NOT NULL DEFAULT '',"
                        "`created_at` datetime NOT NULL DEFAULT CURRENT_TIMESTAMP,"
                        "`data` json DEFAULT NULL,"
                        "`eosto` varchar(12) GENERATED ALWAYS AS (`data` ->> '$.to'),"
                        "`eosfrom` varchar(12) GENERATED ALWAYS AS (`data` ->> '$.from'),"
                        "`receiver` varchar(12) GENERATED ALWAYS AS (`data` ->> '$.receiver'),"
                        "`payer` varchar(12) GENERATED ALWAYS AS (`data` ->> '$.payer'),"
                        "`newaccount` varchar(12) GENERATED ALWAYS AS (`data` ->> '$.name'),"
                        "PRIMARY KEY (`id`),"
                        "KEY `idx_actions_account` (`account`),"
                        "KEY `idx_actions_tx_id` (`transaction_id`),"
                        "KEY `idx_actions_created` (`created_at`),"
                        "KEY `idx_actions_eosto` (`eosto`),"
                        "KEY `idx_actions_eosfrom` (`eosfrom`),"
                        "KEY `idx_actions_receiver` (`receiver`),"
                        "KEY `idx_actions_payer` (`payer`),"
                        "KEY `idx_actions_newaccount` (`newaccount`)"
                        ") ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_general_ci;";

        *m_session << "CREATE TABLE `actions_accounts` ("
                        "`id` bigint(20) NOT NULL AUTO_INCREMENT,"
                        "`actor` varchar(16) COLLATE utf8mb4_general_ci NOT NULL DEFAULT '',"
                        "`permission` varchar(16) COLLATE utf8mb4_general_ci NOT NULL DEFAULT '',"
                        "`action_id` bigint(20) NOT NULL DEFAULT 0,"
                        "PRIMARY KEY (`id`),"
                        "KEY `idx_actions_actor` (`actor`),"
                        "KEY `idx_actions_action_id` (`action_id`)"
                        ") ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_general_ci;";

    }

    void actions_table::add(chain::action action, chain::transaction_id_type transaction_id, fc::time_point_sec transaction_time, uint8_t seq) {

        if(action.name.to_string() == "onblock") return ; //system contract abi haven't onblock, so we could get abi_data.

        chain::abi_def abi;
        std::string abi_def_account;
        chain::abi_serializer abis;
        soci::indicator ind;
        const auto transaction_id_str = transaction_id.str();
        const auto expiration = boost::chrono::seconds{transaction_time.sec_since_epoch()}.count();

        string json = add_data(action);

        boost::uuids::random_generator gen;
        boost::uuids::uuid id = gen();
        std::string action_id = boost::uuids::to_string(id);

        try{
            *m_session << "INSERT INTO actions(account, seq, created_at, name, data, transaction_id) VALUES (:ac, :se, FROM_UNIXTIME(:ca), :na, :da, :ti) ",
                soci::use(action.account.to_string()),
                soci::use(seq),
                soci::use(expiration),
                soci::use(action.name.to_string()),
                soci::use(json),
                soci::use(transaction_id_str);
        }catch(...){
            wlog("insert action failed in ${n}::${a}",("n",action.account.to_string())("a",action.name.to_string()));
            wlog("${data}",("data",fc::json::to_string(action)));
        }

        for (const auto& auth : action.authorization) {
            *m_session << "INSERT INTO actions_accounts(action_id, actor, permission) VALUES (LAST_INSERT_ID(), :ac, :pe) ",
                    soci::use(auth.actor.to_string()),
                    soci::use(auth.permission.to_string());
        }

        try {
            parse_actions( action );
        } catch(std::exception& e){
            wlog(e.what());
        }
    }

    void actions_table::parse_actions( chain::action action ) {
        
        if(action.name == newaccount) {
            auto action_data = action.data_as<chain::newaccount>();
            *m_session << "INSERT INTO accounts (name) VALUES (:name)",
                    soci::use(action_data.name.to_string());

            for (const auto& key_owner : action_data.owner.keys) {
                string permission_owner = "owner";
                string public_key_owner = static_cast<string>(key_owner.key);
                *m_session << "INSERT INTO accounts_keys(account, public_key, permission) VALUES (:ac, :ke, :pe) ",
                        soci::use(action_data.name.to_string()),
                        soci::use(public_key_owner),
                        soci::use(permission_owner);
            }

            for (const auto& key_active : action_data.active.keys) {
                string permission_active = "active";
                string public_key_active = static_cast<string>(key_active.key);
                *m_session << "INSERT INTO accounts_keys(account, public_key, permission) VALUES (:ac, :ke, :pe) ",
                        soci::use(action_data.name.to_string()),
                        soci::use(public_key_active),
                        soci::use(permission_active);
            }

        }
    }


    string actions_table::add_data(chain::action action){
        string json_str = "{}";

        if(action.data.size() ==0 ){
             return json_str;
        }

        try{
            //当为set contract时 存储abi
            if( action.account == chain::config::system_account_name ){
                if( action.name == setabi ){
                    auto setabi = action.data_as<chain::setabi>();
                    try{
                        const chain::abi_def& abi_def = fc::raw::unpack<chain::abi_def>(setabi.abi);
                        json_str = fc::json::to_string( abi_def );

                        try{
                            *m_session << "UPDATE accounts SET abi = :abi, updated_at = NOW() WHERE name = :name",soci::use(json_str),soci::use(setabi.account.to_string());
                            ilog("update abi ${n}",("n",action.account.to_string()));
                        }catch(...){
                            wlog("insert account abi failed");
                        }

                        return json_str;
                    }catch(fc::exception& e){
                        wlog("get setabi data wrong ${e}",("e",e.what()));
                    }
                }
            }

            chain::abi_def abi;
            std::string abi_def_account;
            chain::abi_serializer abis;
            soci::indicator ind;
            //get account abi
            *m_session << "SELECT abi FROM accounts WHERE name = :name", soci::into(abi_def_account, ind), soci::use(action.account.to_string());

            if(!abi_def_account.empty()){
                try {
                    abi = fc::json::from_string(abi_def_account).as<chain::abi_def>();
                    abis.set_abi( abi );
                    auto binary_data = abis.binary_to_variant( abis.get_action_type(action.name), action.data);
                    json_str = fc::json::to_string(binary_data);
                    return json_str;
                } catch(...) {
                    wlog("unable to convert account abi to abi_def for ${s}::${n} :${abi}",("s",action.account)("n",action.name)("abi",action.data));
                    wlog("analysis data failed");
                }
            }

        }catch( std::exception& e ) {
            ilog( "Unable to convert action.data to ABI: ${s}::${n}, std what: ${e}",
                    ("s", action.account)( "n", action.name )( "e", e.what()));
        } catch( ... ) {
            ilog( "Unable to convert action.data to ABI: ${s}::${n}, unknown exception",
                    ("s", action.account)( "n", action.name ));
        }
        return json_str;
    }

    const chain::account_name actions_table::newaccount = "newaccount";
    const chain::account_name actions_table::setabi = "setabi";

} // namespace
