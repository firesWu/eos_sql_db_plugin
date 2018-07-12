/**
 *  @file
 *  @copyright defined in eos/LICENSE.txt
 */

#pragma once

#include <boost/noncopyable.hpp>
#include <boost/chrono.hpp>
#include <boost/signals2/connection.hpp>
#include <boost/thread/thread.hpp>
#include <boost/thread/mutex.hpp>
#include <boost/thread/condition_variable.hpp>
#include <eosio/chain/block_state.hpp>
#include <eosio/chain/transaction.hpp>
#include <fc/log/logger.hpp>
#include <eosio/sql_db_plugin/database.hpp>

// #include "database.hpp"

namespace eosio {

class consumer final : public boost::noncopyable {
    public:
        consumer(std::unique_ptr<database> db, size_t queue_size);
        ~consumer();
        void shutdown();

        template<typename Queue, typename Entry>
        void queue(boost::mutex&, boost::condition_variable&, Queue&, const Entry&, size_t );

        void push_transaction_metadata( const chain::transaction_metadata_ptr& );
        void push_transaction_trace( const chain::transaction_trace_ptr& );
        void push_block_state( const chain::block_state_ptr& );
        void push_irreversible_block_state( const chain::block_state_ptr& );
        void run();

        std::deque<chain::block_state_ptr> block_state_queue;
        std::deque<chain::block_state_ptr> block_state_process_queue;
        std::deque<chain::block_state_ptr> irreversible_block_state_queue;
        std::deque<chain::block_state_ptr> irreversible_block_state_process_queue;
        std::deque<chain::transaction_metadata_ptr> transaction_metadata_queue;
        std::deque<chain::transaction_metadata_ptr> transaction_metadata_process_queue;
        std::deque<chain::transaction_trace_ptr> transaction_trace_queue;
        std::deque<chain::transaction_trace_ptr> transaction_trace_process_queue;

        std::unique_ptr<database> db;
        size_t queue_size;
        boost::atomic<bool> exit{false};
        boost::thread consume_thread;
        boost::mutex mtx;
        boost::condition_variable condition;

    };

    consumer::consumer(std::unique_ptr<database> db, size_t queue_size):
        db(std::move(db)),
        queue_size(queue_size),
        exit(false),
        consume_thread(boost::thread([&]{this->run();})){ }

    consumer::~consumer() {
        exit = true;
        condition.notify_one();
        consume_thread.join();
    }

    void consumer::shutdown() {
        exit = true;
        condition.notify_one();
        consume_thread.join();
    }

    template<typename Queue, typename Entry>
    void consumer::queue(boost::mutex& mtx, boost::condition_variable& condition, Queue& queue, const Entry& e, size_t queue_size) {
        int sleep_time = 100;
        size_t last_queue_size = 0;
        boost::mutex::scoped_lock lock(mtx);
        if (queue.size() > queue_size) {
            lock.unlock();
            condition.notify_one();
            if (last_queue_size < queue.size()) {
                sleep_time += 100;
            } else {
                sleep_time -= 100;
                if (sleep_time < 0) sleep_time = 100;
            }
            last_queue_size = queue.size();
            boost::this_thread::sleep_for(boost::chrono::milliseconds(sleep_time));
            lock.lock();
        }
        queue.emplace_back(e);
        lock.unlock();
        condition.notify_one();
    }

    void consumer::push_block_state( const chain::block_state_ptr& bs ){
        try {
            queue(mtx, condition, block_state_queue, bs, queue_size);
        } catch (fc::exception& e) {
            elog("FC Exception while accepted_block ${e}", ("e", e.to_string()));
        } catch (std::exception& e) {
            elog("STD Exception while accepted_block ${e}", ("e", e.what()));
        } catch (...) {
            elog("Unknown exception while accepted_block");
        }
    }

    void consumer::push_irreversible_block_state( const chain::block_state_ptr& bs ){
        try {
            queue(mtx, condition, irreversible_block_state_queue, bs, queue_size);
        } catch (fc::exception& e) {
            elog("FC Exception while applied_irreversible_block ${e}", ("e", e.to_string()));
        } catch (std::exception& e) {
            elog("STD Exception while applied_irreversible_block ${e}", ("e", e.what()));
        } catch (...) {
            elog("Unknown exception while applied_irreversible_block");
        }
    }

    void consumer::push_transaction_metadata( const chain::transaction_metadata_ptr& tm){
        try {
            queue(mtx, condition, transaction_metadata_queue, tm, queue_size);
        } catch (fc::exception& e) {
            elog("FC Exception while accepted_transaction ${e}", ("e", e.to_string()));
        } catch (std::exception& e) {
            elog("STD Exception while accepted_transaction ${e}", ("e", e.what()));
        } catch (...) {
            elog("Unknown exception while accepted_transaction");
        }
    }

    void consumer::push_transaction_trace( const chain::transaction_trace_ptr& tt){
        try {
            queue(mtx, condition, transaction_trace_queue, tt, queue_size);
        } catch (fc::exception& e) {
            elog("FC Exception while applied_transaction ${e}", ("e", e.to_string()));
        } catch (std::exception& e) {
            elog("STD Exception while applied_transaction ${e}", ("e", e.what()));
        } catch (...) {
            elog("Unknown exception while applied_transaction");
        }
    }
    

    void consumer::run() {

        try{
            dlog("Consumer thread Start");
            while (!exit) { 
                boost::mutex::scoped_lock lock(mtx);
                while(block_state_queue.empty()&&
                        irreversible_block_state_queue.empty()&&
                            transaction_metadata_queue.empty()&&
                                transaction_trace_queue.empty()&& !exit){
                    condition.wait(lock);
                }

                size_t block_state_size = block_state_queue.size();
                if( block_state_size > 0 ){
                    block_state_process_queue = std::move(block_state_queue);
                }

                size_t irreversible_block_state_size = irreversible_block_state_queue.size();
                if( irreversible_block_state_size > 0 ){
                    irreversible_block_state_process_queue = std::move(irreversible_block_state_queue);
                }
                size_t transaction_metadata_size = transaction_metadata_queue.size();
                if( transaction_metadata_size > 0 ){
                    transaction_metadata_process_queue = std::move(transaction_metadata_queue);
                }

                size_t transaction_trace_size = transaction_trace_queue.size();
                if( transaction_trace_size > 0 ){
                    transaction_trace_process_queue = std::move(transaction_trace_queue);
                }

                lock.unlock();

                if( block_state_size > (queue_size * 0.75) ||
                    irreversible_block_state_size > (queue_size * 0.75) ||
                    transaction_metadata_size > (queue_size * 0.75) ||
                    transaction_trace_size> (queue_size * 0.75)) {
                    wlog("queue size: ${q}", ("q", transaction_metadata_size + transaction_trace_size + block_state_size + irreversible_block_state_size));
                } else if (exit) {
                    ilog("draining queue, size: ${q}", ("q", transaction_metadata_size + transaction_trace_size + block_state_size + irreversible_block_state_size));
                }

                // process blocks
                while (!block_state_process_queue.empty()) {
                    const auto& bs = block_state_process_queue.front();
                    db->consume_block_state( bs );
                    block_state_process_queue.pop_front();
                }

                // process irreversible blocks
                while (!irreversible_block_state_process_queue.empty()) {
                    const auto& bs = irreversible_block_state_process_queue.front();
                    db->consume_irreversible_block_state(bs);
                    irreversible_block_state_process_queue.pop_front();
                }

                // process transactions
                while (!transaction_metadata_process_queue.empty()) {
                    const auto& tm = transaction_metadata_process_queue.front();
                    db->consume_transaction_metadata(tm);
                    transaction_metadata_process_queue.pop_front();
                }

                while (!transaction_trace_process_queue.empty()) {
                    const auto& tt = transaction_trace_process_queue.front();
                    db->consume_transaction_trace(tt);
                    transaction_trace_process_queue.pop_front();
                }

            }

        } catch (fc::exception& e) {
            elog("FC Exception while consuming block ${e}", ("e", e.to_string()));
        } catch (std::exception& e) {
            elog("STD Exception while consuming block ${e}", ("e", e.what()));
        } catch (...) {
            elog("Unknown exception while consuming block");
        }
        dlog("Consumer thread End");
    }

} // namespace

