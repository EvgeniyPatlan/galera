//
// Copyright (C) 2011-2017 Codership Oy <info@codership.com>
//


#ifndef GALERA_IST_HPP
#define GALERA_IST_HPP

#include "wsrep_api.h"
#include "galera_gcs.hpp"
#include "trx_handle.hpp"
#include "gu_config.hpp"
#include "gu_lock.hpp"
#include "gu_monitor.hpp"
#include "gu_asio.hpp"

#include <stack>
#include <set>

namespace gcache
{
    class GCache;
}

namespace galera
{
    namespace ist
    {
        void register_params(gu::Config& conf);


        // IST event observer interface
        class EventObserver
        {
        public:
            // Process transaction from IST
            virtual void ist_trx(const TrxHandlePtr&, bool must_apply) = 0;
            // Report IST end
            virtual void ist_end(int error) = 0;
        protected:
            virtual ~EventObserver() {}
        };

        class Receiver
        {
        public:
            static std::string const RECV_ADDR;
            static std::string const RECV_BIND;

            Receiver(gu::Config& conf, gcache::GCache&,
                     TrxHandle::SlavePool& slave_pool,
                     EventObserver&, const char* addr);
            ~Receiver();

            std::string   prepare(wsrep_seqno_t       first_seqno,
                                  wsrep_seqno_t       last_seqno,
                                  int                 protocol_version,
                                  const wsrep_uuid_t& source_id);

            // this must be called AFTER SST is processed and we know
            // the starting point.
            void          ready(wsrep_seqno_t first);

            wsrep_seqno_t finished();
            void          run();

            wsrep_seqno_t current_seqno()   { return current_seqno_; }
            wsrep_seqno_t first_seqno()     { return first_seqno_; }
            wsrep_seqno_t last_seqno()      { return last_seqno_; }
            bool          running()         { return running_; }

        private:

            void interrupt();

            std::string                                   recv_addr_;
            std::string                                   recv_bind_;
            asio::io_service                              io_service_;
            asio::ip::tcp::acceptor                       acceptor_;
            asio::ssl::context                            ssl_ctx_;
#ifdef HAVE_PSI_INTERFACE
            gu::MutexWithPFS                              mutex_;
            gu::CondWithPFS                               cond_;
#else
            gu::Mutex                                     mutex_;
            gu::Cond                                      cond_;
#endif /* HAVE_PSI_INTERFACE */

            wsrep_seqno_t         first_seqno_;
            wsrep_seqno_t         last_seqno_;
            wsrep_seqno_t         current_seqno_;
            gu::Config&           conf_;
            gcache::GCache&       gcache_;
            TrxHandle::SlavePool& slave_pool_;
            wsrep_uuid_t          source_id_;
            EventObserver&        observer_;
            gu_thread_t           thread_;
            int                   error_code_;
            int                   version_;
            bool                  use_ssl_;
            bool                  running_;
            bool                  ready_;

            // GCC 4.8.5 on FreeBSD wants this
            Receiver(const Receiver&);
            Receiver& operator=(const Receiver&);
        };

        class Sender
        {
        public:

            Sender(const gu::Config& conf,
                   gcache::GCache& gcache,
                   const std::string& peer,
                   int version);
            virtual ~Sender();

            // first - first trx seqno
            // last  - last trx seqno
            void send(wsrep_seqno_t first, wsrep_seqno_t last);

            void cancel()
            {
                if (use_ssl_ == true)
                {
                    ssl_stream_->lowest_layer().close();
                }
                else
                {
                    socket_.close();
                }
            }

        private:

            asio::io_service                          io_service_;
            asio::ip::tcp::socket                     socket_;
            asio::ssl::context                        ssl_ctx_;
            asio::ssl::stream<asio::ip::tcp::socket>* ssl_stream_;
            const gu::Config&                         conf_;
            gcache::GCache&                           gcache_;
            int                                       version_;
            bool                                      use_ssl_;

            Sender(const Sender&);
            void operator=(const Sender&);
        };


        class AsyncSender;
        class AsyncSenderMap
        {
        public:
            AsyncSenderMap(gcache::GCache& gcache)
                :
                senders_(),
#ifdef HAVE_PSI_INTERFACE
                monitor_(WSREP_PFS_INSTR_TAG_ASYNC_SENDER_MONITOR_MUTEX,
                         WSREP_PFS_INSTR_TAG_ASYNC_SENDER_MONITOR_CONDVAR),
#else
                monitor_(),
#endif /* HAVE_PSI_INTERFACE */
                gcache_(gcache)
            { }

            void run(const gu::Config& conf,
                     const std::string& peer,
                     wsrep_seqno_t first,
                     wsrep_seqno_t last,
                     int           version);

            void remove(AsyncSender*, wsrep_seqno_t);
            void cancel();
            gcache::GCache& gcache() { return gcache_; }
        private:
            std::set<AsyncSender*> senders_;
            // use monitor instead of mutex, it provides cancellation point
            gu::Monitor            monitor_;
            gcache::GCache&        gcache_;
        };


    } // namespace ist
} // namespace galera

#endif // GALERA_IST_HPP
