//
// Copyright (C) 2010-2017 Codership Oy <info@codership.com>
//

#include "galera_common.hpp"
#include "replicator_smm.hpp"
#include "galera_exception.hpp"

#include "galera_info.hpp"

#include <gu_debug_sync.hpp>
#include <gu_abort.h>

#include <sstream>
#include <iostream>


std::ostream& galera::operator<<(std::ostream& os, ReplicatorSMM::State state)
{
    switch (state)
    {
    case ReplicatorSMM::S_DESTROYED: return (os << "DESTROYED");
    case ReplicatorSMM::S_CLOSED:    return (os << "CLOSED");
    case ReplicatorSMM::S_CONNECTED: return (os << "CONNECTED");
    case ReplicatorSMM::S_JOINING:   return (os << "JOINING");
    case ReplicatorSMM::S_JOINED:    return (os << "JOINED");
    case ReplicatorSMM::S_SYNCED:    return (os << "SYNCED");
    case ReplicatorSMM::S_DONOR:     return (os << "DONOR");
    }

    gu_throw_fatal << "invalid state " << static_cast<int>(state);
}

//////////////////////////////////////////////////////////////////////
//////////////////////////////////////////////////////////////////////
//                           Public
//////////////////////////////////////////////////////////////////////
//////////////////////////////////////////////////////////////////////

galera::ReplicatorSMM::ReplicatorSMM(const struct wsrep_init_args* args)
    :
    ist_event_queue_    (),
    init_lib_           (reinterpret_cast<gu_log_cb_t>(args->logger_cb),
                         reinterpret_cast<gu_pfs_instr_cb_t>(args->pfs_instr_cb)),
    config_             (),
    init_config_        (config_, args->node_address, args->data_dir),
    parse_options_      (*this, config_, args->options),
    init_ssl_           (config_),
    str_proto_ver_      (-1),
    protocol_version_   (-1),
    proto_max_          (gu::from_string<int>(config_.get(Param::proto_max))),
    state_              (S_CLOSED),
    closing_mutex_      (),
    closing_cond_       (),
    closing_            (false),
    sst_state_          (SST_NONE),
    co_mode_            (CommitOrder::from_string(
                             config_.get(Param::commit_order))),
    state_file_         (config_.get(BASE_DIR)+'/'+GALERA_STATE_FILE),
    st_                 (state_file_),
    safe_to_bootstrap_  (true),
    trx_params_         (config_.get(BASE_DIR), -1,
                         KeySet::version(config_.get(Param::key_format)),
                         TrxHandle::Defaults.record_set_ver_,
                         gu::from_string<int>(config_.get(
                             Param::max_write_set_size))),
    uuid_               (WSREP_UUID_UNDEFINED),
    state_uuid_         (WSREP_UUID_UNDEFINED),
    state_uuid_str_     (),
    cc_seqno_           (WSREP_SEQNO_UNDEFINED),
    pause_seqno_        (WSREP_SEQNO_UNDEFINED),
    app_ctx_            (args->app_ctx),
    connected_cb_       (args->connected_cb),
    view_cb_            (args->view_cb),
    sst_request_cb_     (args->sst_request_cb),
    apply_cb_           (args->apply_cb),
    commit_cb_          (args->commit_cb),
    unordered_cb_       (args->unordered_cb),
    sst_donate_cb_      (args->sst_donate_cb),
    synced_cb_          (args->synced_cb),
    abort_cb_           (args->abort_cb),
    sst_donor_          (),
    sst_uuid_           (WSREP_UUID_UNDEFINED),
    sst_seqno_          (WSREP_SEQNO_UNDEFINED),
#ifdef HAVE_PSI_INTERFACE
    sst_mutex_          (WSREP_PFS_INSTR_TAG_SST_MUTEX),
    sst_cond_           (WSREP_PFS_INSTR_TAG_SST_CONDVAR),
#else
    sst_mutex_          (),
    sst_cond_           (),
#endif /* HAVE_PSI_INTERFACE */
    sst_retry_sec_      (1),
    sst_received_       (false),
    gcache_             (config_, config_.get(BASE_DIR)),
    gcs_                (config_, gcache_, proto_max_, args->proto_ver,
                         args->node_name, args->node_incoming),
    service_thd_        (gcs_, gcache_),
    slave_pool_         (sizeof(TrxHandle), 1024, "SlaveTrxHandle"),
    as_                 (new GcsActionSource(slave_pool_, gcs_, *this, gcache_)),
    ist_receiver_       (config_, gcache_, slave_pool_, *this, args->node_address),
    ist_senders_        (gcache_),
    ist_prepared_       (false),
    wsdb_               (),
    cert_               (config_, service_thd_, gcache_),
#ifdef HAVE_PSI_INTERFACE
    local_monitor_      (WSREP_PFS_INSTR_TAG_LOCAL_MONITOR_MUTEX,
                         WSREP_PFS_INSTR_TAG_LOCAL_MONITOR_CONDVAR),
    apply_monitor_      (WSREP_PFS_INSTR_TAG_APPLY_MONITOR_MUTEX,
                         WSREP_PFS_INSTR_TAG_APPLY_MONITOR_CONDVAR),
    commit_monitor_     (WSREP_PFS_INSTR_TAG_COMMIT_MONITOR_MUTEX,
                         WSREP_PFS_INSTR_TAG_COMMIT_MONITOR_CONDVAR),
#else
    local_monitor_      (),
    apply_monitor_      (),
    commit_monitor_     (),
#endif /* HAVE_PSI_INTERFACE */
    causal_read_timeout_(config_.get(Param::causal_read_timeout)),
    receivers_          (),
    replicated_         (),
    replicated_bytes_   (),
    keys_count_         (),
    keys_bytes_         (),
    data_bytes_         (),
    unrd_bytes_         (),
    local_commits_      (),
    local_rollbacks_    (),
    local_cert_failures_(),
    local_replays_      (),
    causal_reads_       (),
    preordered_id_      (),
    incoming_list_      (""),
#ifdef HAVE_PSI_INTERFACE
    incoming_mutex_     (WSREP_PFS_INSTR_TAG_INCOMING_MUTEX),
#else
    incoming_mutex_     (),
#endif /* HAVE_PSI_INTERFACE */
    wsrep_stats_        ()
{
    /*
      Register the application callback that should be called
      if the wsrep provider will teminated abnormally:
    */
    if (abort_cb_)
    {
        gu_abort_register_cb(abort_cb_);
    }

    // @todo add guards (and perhaps actions)
    state_.add_transition(Transition(S_CLOSED,  S_DESTROYED));
    state_.add_transition(Transition(S_CLOSED,  S_CONNECTED));

    state_.add_transition(Transition(S_CONNECTED, S_CLOSED));
    state_.add_transition(Transition(S_CONNECTED, S_CONNECTED));
    state_.add_transition(Transition(S_CONNECTED, S_JOINING));
    // the following is possible only when bootstrapping new cluster
    // (trivial wsrep_cluster_address)
    state_.add_transition(Transition(S_CONNECTED, S_JOINED));
    // the following are possible on PC remerge
    state_.add_transition(Transition(S_CONNECTED, S_DONOR));
    state_.add_transition(Transition(S_CONNECTED, S_SYNCED));

    state_.add_transition(Transition(S_JOINING, S_CLOSED));
    // the following is possible if one non-prim conf follows another
    state_.add_transition(Transition(S_JOINING, S_CONNECTED));
    state_.add_transition(Transition(S_JOINING, S_JOINED));

    state_.add_transition(Transition(S_JOINED, S_CLOSED));
    state_.add_transition(Transition(S_JOINED, S_CONNECTED));
    state_.add_transition(Transition(S_JOINED, S_SYNCED));
    // the following is possible if one desync() immediately follows another
    state_.add_transition(Transition(S_JOINED, S_DONOR));

    state_.add_transition(Transition(S_SYNCED, S_CLOSED));
    state_.add_transition(Transition(S_SYNCED, S_CONNECTED));
    state_.add_transition(Transition(S_SYNCED, S_DONOR));

    state_.add_transition(Transition(S_DONOR, S_CLOSED));
    state_.add_transition(Transition(S_DONOR, S_CONNECTED));
    state_.add_transition(Transition(S_DONOR, S_JOINED));

    local_monitor_.set_initial_position(WSREP_UUID_UNDEFINED, 0);

    wsrep_uuid_t  uuid;
    wsrep_seqno_t seqno;

    st_.get (uuid, seqno, safe_to_bootstrap_);

    if (0 != args->state_id &&
        args->state_id->uuid != WSREP_UUID_UNDEFINED &&
        args->state_id->uuid == uuid                 &&
        seqno                == WSREP_SEQNO_UNDEFINED)
    {
        /* non-trivial recovery information provided on startup, and db is safe
         * so use recovered seqno value */
        seqno = args->state_id->seqno;
    }

    log_debug << "End state: " << uuid << ':' << seqno << " #################";

    // We need to set the current value of uuid and update
    // stored seqno value, if the non-trivial recovery
    // information provided on startup:

    // this will persist the recover/valid co-ordinates to grastate.dat
    // Following logic that plan to put node in an inconsistent state can update
    // the grastate to mark as unsafe and then safe (on successful completion).
    set_initial_position(uuid, seqno);
    gcache_.seqno_reset(gu::GTID(uuid, seqno));
    // update gcache position to one supplied by app.

    build_stats_vars(wsrep_stats_);
}

void galera::ReplicatorSMM::start_closing()
{
    assert(closing_mutex_.locked());
    assert(state_() >= S_CONNECTED);
    if (!closing_)
    {
        closing_ = true;
        gcs_.close();
    }
}

void galera::ReplicatorSMM::shift_to_CLOSED()
{
    assert(closing_mutex_.locked());
    assert(closing_);

    state_.shift_to(S_CLOSED);

    /* Cleanup for re-opening. */
    uuid_ = WSREP_UUID_UNDEFINED;
    closing_ = false;
    if (st_.corrupt())
    {
        /* this is a synchronization hack to make sure all receivers are done
         * with their work and won't access cert module any more. The usual
         * monitor drain is not enough here. */
        while (receivers_() > 1) usleep(1000);

        // this should erase the memory of a pre-existing state.
        set_initial_position(WSREP_UUID_UNDEFINED, WSREP_SEQNO_UNDEFINED);
        cert_.assign_initial_position(gu::GTID(GU_UUID_NIL, -1),
                                      trx_params_.version_);
        sst_uuid_            = WSREP_UUID_UNDEFINED;
        sst_seqno_           = WSREP_SEQNO_UNDEFINED;
        cc_seqno_            = WSREP_SEQNO_UNDEFINED;
        pause_seqno_         = WSREP_SEQNO_UNDEFINED;
    }

    closing_cond_.broadcast();
}

void galera::ReplicatorSMM::wait_for_CLOSED(gu::Lock& lock)
{
    assert(closing_mutex_.locked());
    assert(closing_);
    while (state_() > S_CLOSED) lock.wait(closing_cond_);
    assert(!closing_);
    assert(WSREP_UUID_UNDEFINED == uuid_);
}

galera::ReplicatorSMM::~ReplicatorSMM()
{
    log_debug << "dtor state: " << state_();

    gu::Lock lock(closing_mutex_);

    switch (state_())
    {
    case S_CONNECTED:
    case S_JOINING:
    case S_JOINED:
    case S_SYNCED:
    case S_DONOR:
        start_closing();
        wait_for_CLOSED(lock);
        // @todo wait that all users have left the building
        // fall through
    case S_CLOSED:
        ist_senders_.cancel();
        break;
    case S_DESTROYED:
        break;
    }

    delete as_;
}


wsrep_status_t galera::ReplicatorSMM::connect(const std::string& cluster_name,
                                              const std::string& cluster_url,
                                              const std::string& state_donor,
                                              bool  const        bootstrap)
{
    sst_donor_ = state_donor;
    service_thd_.reset();

    ssize_t err = 0;
    wsrep_status_t ret(WSREP_OK);
    wsrep_seqno_t const seqno(STATE_SEQNO());
    wsrep_uuid_t  const gcs_uuid(seqno < 0 ? WSREP_UUID_UNDEFINED :state_uuid_);

    log_info << "Setting GCS initial position to " << gcs_uuid << ':' << seqno;

    if ((bootstrap == true || cluster_url == "gcomm://")
        && safe_to_bootstrap_ == false)
    {
        log_error << "It may not be safe to bootstrap the cluster from this node. "
                  << "It was not the last one to leave the cluster and may "
                  << "not contain all the updates. To force cluster bootstrap "
                  << "with this node, edit the grastate.dat file manually and "
                  << "set safe_to_bootstrap to 1 .";
        ret = WSREP_NODE_FAIL;
    }

    if (ret == WSREP_OK &&
        (err = gcs_.set_initial_position(gu::GTID(gcs_uuid, seqno))) != 0)
    {
        log_error << "gcs init failed:" << strerror(-err);
        ret = WSREP_NODE_FAIL;
    }

    if (ret == WSREP_OK &&
        (err = gcs_.connect(cluster_name, cluster_url, bootstrap)) != 0)
    {
        log_error << "gcs connect failed: " << strerror(-err);
        ret = WSREP_NODE_FAIL;
    }

    if (ret == WSREP_OK)
    {
        state_.shift_to(S_CONNECTED);
    }

    return ret;
}


wsrep_status_t galera::ReplicatorSMM::close()
{
    // We must be sure that IST receiver will be stopped,
    // even if the IST during the execution:
    if (ist_prepared_)
    {
        ist_prepared_ = false;
        sst_seqno_ = ist_receiver_.finished();
    }

    gu::Lock lock(closing_mutex_);

    if (state_() > S_CLOSED)
    {
        start_closing();
        wait_for_CLOSED(lock);
    }

    return WSREP_OK;
}


wsrep_status_t galera::ReplicatorSMM::async_recv(void* recv_ctx)
{
    assert(recv_ctx != 0);

    if (state_() <= S_CLOSED)
    {
        log_error <<"async recv cannot start, provider in CLOSED state";
        return WSREP_FATAL;
    }

    ++receivers_;

    bool exit_loop(false);
    wsrep_status_t retval(WSREP_OK);

    while (WSREP_OK == retval && state_() > S_CLOSED)
    {
        ssize_t rc;

        while (gu_unlikely((rc = as_->process(recv_ctx, exit_loop))
                           == -ECANCELED))
        {
            recv_IST(recv_ctx);
            // hack: prevent fast looping until ist controlling thread
            // resumes gcs prosessing
            usleep(10000);
        }

        if (gu_unlikely(rc <= 0))
        {
            retval = WSREP_CONN_FAIL;
        }
        else if (gu_unlikely(exit_loop == true))
        {
            assert(WSREP_OK == retval);

            if (receivers_.sub_and_fetch(1) > 0)
            {
                log_info << "Slave thread exiting on request.";
                break;
            }

            ++receivers_;
            log_warn << "Refusing exit for the last slave thread.";
        }
    }

    /* exiting loop already did proper checks */
    if (!exit_loop && receivers_.sub_and_fetch(1) == 0)
    {
        gu::Lock lock(closing_mutex_);

        if (state_() > S_CLOSED && !closing_)
        {
            if (retval == WSREP_OK)
            {
                log_warn << "Broken shutdown sequence, provider state: "
                         << state_() << ", retval: " << retval;
                assert (0);
            }
            else
            {
                // Generate zero view before exit to notify application
                wsrep_view_info_t* err_view(galera_view_info_create(0, false));
                view_cb_(app_ctx_, recv_ctx, err_view, 0, 0);
                free(err_view);
            }

            shift_to_CLOSED();
        }
    }

    log_debug << "Slave thread exit. Return code: " << retval;

    return retval;
}


void galera::ReplicatorSMM::apply_trx(void* recv_ctx, TrxHandle& trx)
{
    assert(trx.global_seqno() > 0);
    if (!trx.skip_event())
    {
        assert(trx.trx_id() != uint64_t(-1) || trx.is_toi());
        assert(trx.is_certified() /*Repl*/ || trx.preordered() /*IST*/);
        assert(trx.is_local() == false ||
               (trx.flags() & TrxHandle::F_ROLLBACK));
    }

    ApplyException ae;

    ApplyOrder ao(trx);
    CommitOrder co(trx, co_mode_);

    uint32_t commit_flags(TrxHandle::trx_flags_to_wsrep_flags(trx.flags()));
    bool const aborting(TrxHandle::S_ABORTING == trx.state());
    bool const applying(!aborting || trx.pa_unsafe());

    if (gu_likely(applying))
    {
        gu_trace(apply_monitor_.enter(ao));
    }

    trx.set_state(TrxHandle::S_APPLYING);

    wsrep_trx_meta_t meta = { {state_uuid_, trx.global_seqno() },
                              { trx.source_id(), trx.trx_id(), trx.conn_id() },
                              trx.depends_seqno() };

    if (trx.is_toi())
    {
        log_debug << "Executing TO isolated action: " << trx;
        st_.mark_unsafe();
    }

    try { gu_trace(trx.apply(recv_ctx, apply_cb_, meta)); }
    catch (ApplyException& e)
    {
        assert(0 != e.status());
        assert(NULL != e.data() || 0 == e.data_len());
        assert(0 != e.data_len() || NULL == e.data());

        if (trx.is_toi())
        {
            log_warn << "Ignoring error for TO isolated action: " << trx;
            e.free();
        }
        else
        {
            ae = e;
        }
    }
    /* at this point any other exception is fatal, not catching anything else. */

    wsrep_bool_t exit_loop(false);

    TrxHandle* commit_trx_handle = &trx;
    if (gu_likely(co_mode_ != CommitOrder::BYPASS && trx.is_toi()))
    {
        /* TOI action are fully serialized so it is make sense to
        enforce commit ordering at this stage. For non-TOI action
        commit ordering is delayed to take advantage of full parallelism. */
        gu_trace(commit_monitor_.enter(co));
        assert(trx.global_seqno() > STATE_SEQNO());
        commit_trx_handle = NULL;
    }
    trx.set_state(TrxHandle::S_COMMITTING);

    TrxHandle::State end_state(aborting ?
                               TrxHandle::S_ROLLED_BACK :TrxHandle::S_COMMITTED);

    if (gu_likely(0 == ae.status()))
    {
        assert(NULL == ae.data());
        assert(0    == ae.data_len());
    }
    else
    {
        assert(NULL == ae.data() || ae.data_len() > 0);
        commit_flags |= WSREP_FLAG_ROLLBACK;
        end_state = TrxHandle::S_ROLLED_BACK;

        if (!st_.corrupt()) mark_corrupt_and_close();

        ae.free();
    }

    /* Pass trx handle so that commit monitor can be started at later stage */
    wsrep_cb_status_t const rcode(commit_cb_(
	recv_ctx, commit_trx_handle, commit_flags, &meta, &exit_loop));

    if (gu_unlikely (rcode != WSREP_CB_SUCCESS))
        gu_throw_fatal << (commit_flags & WSREP_FLAG_ROLLBACK ?
                           "Rollback" : "Commit")
                       << " failed. Trx: " << trx;

    log_debug << "Trx " << (commit_flags & WSREP_FLAG_ROLLBACK ?
                            "rolled back " : "committed ")
              << trx.global_seqno();

    wsrep_seqno_t safe_to_discard(WSREP_SEQNO_UNDEFINED);

    if (gu_likely(co_mode_ != CommitOrder::BYPASS))
    {
        if (gu_unlikely(!applying))
            /* trx must be set committed while in at least one monitor */
            safe_to_discard = cert_.set_trx_committed(trx);

        if (trx.is_toi())
        {
            commit_monitor_.leave(co);

            // Allow tests to block the applier thread using the DBUG facilities
            GU_DBUG_SYNC_WAIT("sync.apply_trx.after_commit_leave");
        }
    }

    trx.set_state(end_state);

    if (trx.is_local() == false)
    {
        GU_DBUG_SYNC_WAIT("after_commit_slave_sync");
    }

    if (gu_likely(applying))
    {
        /* For now need to keep it inside apply monitor to ensure all processing
         * ends by the time monitors are drained because of potential gcache
         * cleanup (and loss of the writeset buffer). Perhaps unordered monitor
         * is needed here. */
        trx.unordered(recv_ctx, unordered_cb_);

        safe_to_discard = cert_.set_trx_committed(trx);

        apply_monitor_.leave(ao);
    }

    if (trx.is_toi())
    {
        log_debug << "Done executing TO isolated action: "
                  << trx.global_seqno();
        st_.mark_safe();
    }

    if (gu_likely(trx.local_seqno() != -1))
    {
        // trx with local seqno -1 originates from IST (or other source not gcs)
        report_last_committed(safe_to_discard);
    }

    trx.set_exit_loop(exit_loop);
}


wsrep_status_t galera::ReplicatorSMM::send(TrxHandle*        trx,
                                           wsrep_trx_meta_t* meta)
{
    assert(trx->locked());

    if (state_() < S_JOINED) return WSREP_TRX_FAIL;

    const bool rollback(trx->flags() & TrxHandle::F_ROLLBACK);

    if (rollback)
    {
        assert(trx->state() == TrxHandle::S_ABORTING);
    }

    WriteSetNG::GatherVector actv;

    size_t act_size = trx->gather(actv);
    ssize_t rcode(0);
    do
    {
        const bool scheduled(!rollback);

        if (scheduled)
        {
            const ssize_t gcs_handle(gcs_.schedule());

            if (gu_unlikely(gcs_handle < 0))
            {
                log_debug << "gcs schedule " << strerror(-gcs_handle);
                rcode = gcs_handle;
                goto out;
            }
            trx->set_gcs_handle(gcs_handle);
        }

        trx->finalize(last_committed());
        trx->unlock();
        // rollbacks must be sent bypassing SM monitor to avoid deadlocks
        const bool grab(rollback);
        rcode = gcs_.sendv(actv, act_size,
                           GCS_ACT_TORDERED,
                           scheduled, grab);
        GU_DBUG_SYNC_WAIT("after_send_sync");
        trx->lock();
    }
    // TODO: Break loop after some timeout
    while (rcode == -EAGAIN && (usleep(1000), true));

    trx->set_gcs_handle(-1);

out:

    if (rcode <= 0)
    {
        log_debug << "ReplicatorSMM::send failed: " << -rcode;
    }

    return (rcode > 0 ? WSREP_OK : WSREP_TRX_FAIL);
}


wsrep_status_t galera::ReplicatorSMM::replicate(TrxHandlePtr& txp,
                                                wsrep_trx_meta_t* meta)
{
    if (state_() < S_JOINED) return WSREP_TRX_FAIL;

    TrxHandle* const trx(txp.get());

    assert(trx->state() == TrxHandle::S_EXECUTING ||
           trx->state() == TrxHandle::S_MUST_ABORT);
    assert(trx->local_seqno() == WSREP_SEQNO_UNDEFINED &&
           trx->global_seqno() == WSREP_SEQNO_UNDEFINED);

    if (state_() < S_JOINED || trx->state() == TrxHandle::S_MUST_ABORT)
    {
    must_abort:
        if (trx->state() == TrxHandle::S_EXECUTING ||
            trx->state() == TrxHandle::S_REPLICATING)
            trx->set_state(TrxHandle::S_MUST_ABORT);

        trx->set_state(TrxHandle::S_ABORTING);

        return WSREP_CONN_FAIL;
    }

    WriteSetNG::GatherVector actv;

    gcs_action act;
    act.type = GCS_ACT_TORDERED;
#ifndef NDEBUG
    act.seqno_g = GCS_SEQNO_ILL;
#endif
    act.buf  = NULL;
    act.size = trx->gather(actv);
    trx->set_state(TrxHandle::S_REPLICATING);

    ssize_t rcode(-1);

    do
    {
        assert(act.seqno_g == GCS_SEQNO_ILL);

        const ssize_t gcs_handle(gcs_.schedule());

        if (gu_unlikely(gcs_handle < 0))
        {
            log_debug << "gcs schedule " << strerror(-gcs_handle);
            trx->set_state(TrxHandle::S_MUST_ABORT);
            goto must_abort;
        }

        trx->set_gcs_handle(gcs_handle);

        trx->finalize(last_committed());
        assert(trx->last_seen_seqno() >= 0);
        trx->unlock();
        assert (act.buf == NULL); // just a sanity check
        rcode = gcs_.replv(actv, act, true);
        GU_DBUG_SYNC_WAIT("after_replicate_sync")
        trx->lock();
    }
    while (rcode == -EAGAIN && trx->state() != TrxHandle::S_MUST_ABORT &&
           (usleep(1000), true));

    assert(trx->last_seen_seqno() >= 0);
    trx->set_gcs_handle(-1);

    if (rcode < 0)
    {
        if (rcode != -EINTR)
        {
            log_debug << "gcs_repl() failed with " << strerror(-rcode)
                      << " for trx " << *trx;
        }

        assert(rcode != -EINTR || trx->state() == TrxHandle::S_MUST_ABORT);
        assert(act.seqno_l == GCS_SEQNO_ILL && act.seqno_g == GCS_SEQNO_ILL);
        assert(NULL == act.buf);

        if (trx->state() != TrxHandle::S_MUST_ABORT)
        {
            trx->set_state(TrxHandle::S_MUST_ABORT);
        }

        goto must_abort;
    }

    assert(act.buf != NULL);
    assert(act.size == rcode);
    assert(act.seqno_l != GCS_SEQNO_ILL);
    assert(act.seqno_g != GCS_SEQNO_ILL);

    ++replicated_;
    replicated_bytes_ += rcode;
    trx->set_gcs_handle(-1);

    gu_trace(trx->unserialize(static_cast<const gu::byte_t*>(act.buf), act.size, 0));

    trx->update_stats(keys_count_, keys_bytes_, data_bytes_, unrd_bytes_);

    wsrep_status_t retval(WSREP_TRX_FAIL);

    // ROLLBACK event shortcut to avoid blocking in monitors or
    // getting BF aborted inside provider
    if (gu_unlikely(trx->flags() & TrxHandle::F_ROLLBACK))
    {
        assert(trx->depends_seqno() > 0); // must be set at unserialization
        trx->mark_certified();
        gcache_.seqno_assign(trx->action(), trx->global_seqno(),
                             GCS_ACT_TORDERED, false);
        cancel_monitors<true>(*trx);

        trx->set_state(TrxHandle::S_MUST_ABORT);
        trx->set_state(TrxHandle::S_ABORTING);

        goto out;
    }

    trx->set_received(act.buf, act.seqno_l, act.seqno_g);

    if (gu_unlikely(trx->state() == TrxHandle::S_MUST_ABORT))
    {
        retval = cert_for_aborted(txp);

        if (retval != WSREP_BF_ABORT)
        {
            cancel_monitors<true>(*trx);

            assert(trx->is_dummy());
            assert(WSREP_OK != retval);
            retval = WSREP_TRX_FAIL;
        }
        else
        {
            trx->set_state(TrxHandle::S_MUST_CERT_AND_REPLAY);
        }
    }
    else
    {
        assert(trx->state() == TrxHandle::S_REPLICATING);
        retval = WSREP_OK;
    }

    assert(trx->last_seen_seqno() >= 0);

out:
    assert(trx->state() != TrxHandle::S_MUST_ABORT);
    assert(trx->global_seqno() >  0);
    assert(trx->global_seqno() == act.seqno_g);

    if (meta != 0) // whatever the retval, we must update GTID in meta
    {
        meta->gtid.uuid  = state_uuid_;
        meta->gtid.seqno = trx->global_seqno();
        meta->depends_on = trx->depends_seqno();
    }

    return retval;
}

void
galera::ReplicatorSMM::abort_trx(TrxHandle* trx)
{
    assert(trx != 0);
    assert(trx->is_local() == true);

    log_debug << "aborting trx " << *trx << " " << trx;

    switch (trx->state())
    {
    case TrxHandle::S_MUST_ABORT:
    case TrxHandle::S_ABORTING: // guess this is here because we can have a race
        return;
    case TrxHandle::S_EXECUTING:
        trx->set_state(TrxHandle::S_MUST_ABORT);
        break;
    case TrxHandle::S_REPLICATING:
    {
        trx->set_state(TrxHandle::S_MUST_ABORT);
        // trx is in gcs repl
        int rc;
        if (trx->gcs_handle() > 0 &&
            ((rc = gcs_.interrupt(trx->gcs_handle()))) != 0)
        {
            log_debug << "gcs_interrupt(): handle "
                      << trx->gcs_handle()
                      << " trx id " << trx->trx_id()
                      << ": " << strerror(-rc);
        }
        break;
    }
    case TrxHandle::S_CERTIFYING:
    {
        trx->set_state(TrxHandle::S_MUST_ABORT);
        // trx is waiting in local monitor
        LocalOrder lo(*trx);
        trx->unlock();
        local_monitor_.interrupt(lo);
        trx->lock();
        break;
    }
    case TrxHandle::S_APPLYING:
    {
        trx->set_state(TrxHandle::S_MUST_ABORT);
        // trx is waiting in apply monitor
        ApplyOrder ao(*trx);
        trx->unlock();
        apply_monitor_.interrupt(ao);
        trx->lock();
        break;
    }
    case TrxHandle::S_COMMITTING:
        trx->set_state(TrxHandle::S_MUST_ABORT);
        if (co_mode_ != CommitOrder::BYPASS)
        {
            // trx waiting in commit monitor
            CommitOrder co(*trx, co_mode_);
            trx->unlock();
            commit_monitor_.interrupt(co);
            trx->lock();
        }
        break;
    default:
        gu_throw_fatal << "invalid state " << trx->state();
    }
}


wsrep_status_t galera::ReplicatorSMM::pre_commit(TrxHandlePtr&     trx,
                                                 wsrep_trx_meta_t* meta)
{
    /* Replicate and pre-commit action are 2 different actions now.
    This means transaction can get aborted on completion of replicate
    before pre-commit action start. This condition capture that scenario
    and ensure that resources are released. */
    if (trx->state() == TrxHandle::S_MUST_ABORT)
    {
        wsrep_status_t retval(WSREP_PRECOMMIT_ABORT);
        retval = cert_for_aborted(trx);

        if (retval != WSREP_BF_ABORT)
        {
            cancel_monitors<true>(*trx);
        }
        else if (meta != 0)
        {
            meta->gtid.uuid  = state_uuid_;
            meta->gtid.seqno = trx->global_seqno();
            meta->depends_on = trx->depends_seqno();
        }

        if (trx->state() == TrxHandle::S_MUST_ABORT)
            trx->set_state(TrxHandle::S_ABORTING);

        return retval;
    }

    assert(trx->state() == TrxHandle::S_REPLICATING);
    trx->set_state(TrxHandle::S_CERTIFYING);

    assert(trx->local_seqno()  > -1);
    assert(trx->global_seqno() > -1);
    assert(trx->last_seen_seqno() >= 0);

    if (meta != 0)
    {
        meta->gtid.uuid  = state_uuid_;
        meta->gtid.seqno = trx->global_seqno();
        meta->depends_on = trx->depends_seqno();
    }
    // State should not be checked here: If trx has been replicated,
    // it has to be certified and potentially applied. #528
    // if (state_() < S_JOINED) return WSREP_TRX_FAIL;

    wsrep_status_t retval(cert_and_catch(trx));

    assert((trx->flags() & TrxHandle::F_ROLLBACK) == 0 ||
           trx->state() == TrxHandle::S_ABORTING);

    if (gu_unlikely(retval != WSREP_OK))
    {
        switch(retval)
        {
        case WSREP_BF_ABORT:
            assert(trx->depends_seqno() >= 0);
            assert(trx->state() == TrxHandle::S_MUST_CERT_AND_REPLAY ||
                   trx->state() == TrxHandle::S_MUST_REPLAY_AM);
            break;
        case WSREP_TRX_FAIL:
            assert(trx->depends_seqno() < 0);
            assert(trx->state() == TrxHandle::S_ABORTING);
            break;
        default:
            assert(0);
        }

        return retval;
    }

    assert(trx->state() == TrxHandle::S_CERTIFYING);
    assert(trx->global_seqno() > STATE_SEQNO());
    assert(trx->depends_seqno() >= 0);

    trx->set_state(TrxHandle::S_APPLYING);

    ApplyOrder ao(*trx);
    CommitOrder co(*trx, co_mode_);
    bool interrupted(false);

    try
    {
        trx->unlock();
        gu_trace(apply_monitor_.enter(ao));
        GU_DBUG_SYNC_WAIT("after_pre_commit_apply_monitor_enter");
        trx->lock();
        assert(trx->state() == TrxHandle::S_APPLYING ||
               trx->state() == TrxHandle::S_MUST_ABORT);
    }
    catch (gu::Exception& e)
    {
        trx->lock();
        if (e.get_errno() == EINTR)
        {
            interrupted = true;
        }
        else throw;
    }

    if (gu_unlikely(interrupted || trx->state() == TrxHandle::S_MUST_ABORT))
    {
        assert(trx->state() == TrxHandle::S_MUST_ABORT);
        assert(trx->flags() & TrxHandle::F_COMMIT);
        if (interrupted) trx->set_state(TrxHandle::S_MUST_REPLAY_AM);
        else             trx->set_state(TrxHandle::S_MUST_REPLAY_CM);
        retval = WSREP_BF_ABORT;
    }
    else
    {
        assert((trx->flags() & TrxHandle::F_COMMIT) != 0);
        assert(apply_monitor_.entered(ao));

        trx->set_state(TrxHandle::S_COMMITTING);

        if (co_mode_ != CommitOrder::BYPASS)
        {
            try
            {
                trx->unlock();
                gu_trace(commit_monitor_.enter(co));
                trx->lock();
                assert(trx->state() == TrxHandle::S_COMMITTING ||
                       trx->state() == TrxHandle::S_MUST_ABORT);
            }
            catch (gu::Exception& e)
            {
                trx->lock();
                if (e.get_errno() == EINTR) { interrupted = true; }
                else throw;
            }

            if (gu_unlikely(interrupted) ||
                trx->state() == TrxHandle::S_MUST_ABORT)
            {
                assert(trx->state() == TrxHandle::S_MUST_ABORT);
                if (interrupted) trx->set_state(TrxHandle::S_MUST_REPLAY_CM);
                else             trx->set_state(TrxHandle::S_MUST_REPLAY);
                retval = WSREP_BF_ABORT;
            }
        }
    }

    assert((retval == WSREP_OK && (trx->state() == TrxHandle::S_COMMITTING ||
                                   trx->state() == TrxHandle::S_EXECUTING))
           ||
           (retval == WSREP_TRX_FAIL && trx->state() == TrxHandle::S_ABORTING)
           ||
           (retval == WSREP_BF_ABORT && (
               trx->state() == TrxHandle::S_MUST_REPLAY_AM ||
               trx->state() == TrxHandle::S_MUST_REPLAY_CM ||
               trx->state() == TrxHandle::S_MUST_REPLAY))
           ||
           (retval == WSREP_TRX_FAIL && trx->state() == TrxHandle::S_ABORTING)
        );

    if (meta) meta->depends_on = trx->depends_seqno();

    return retval;
}

wsrep_status_t galera::ReplicatorSMM::replay_trx(TrxHandlePtr& txp, void* trx_ctx)
{
    TrxHandle* const trx(txp.get());

    assert(trx->state() == TrxHandle::S_MUST_CERT_AND_REPLAY ||
           trx->state() == TrxHandle::S_MUST_REPLAY_AM       ||
           trx->state() == TrxHandle::S_MUST_REPLAY_CM       ||
           trx->state() == TrxHandle::S_MUST_REPLAY);
    assert(trx->trx_id() != static_cast<wsrep_trx_id_t>(-1));
    assert(trx->global_seqno() > STATE_SEQNO());

    wsrep_status_t retval(WSREP_OK);

    switch (trx->state())
    {
    case TrxHandle::S_MUST_CERT_AND_REPLAY:
        retval = cert_and_catch(txp);
        if (retval != WSREP_OK)
        {
            assert(retval == WSREP_TRX_FAIL);
            assert(trx->state() == TrxHandle::S_ABORTING);
            // apply monitor is self canceled in cert
            break;
        }
        trx->set_state(TrxHandle::S_MUST_REPLAY_AM);
        // fall through
    case TrxHandle::S_MUST_REPLAY_AM:
    {
        // safety measure to make sure that all preceding trxs finish before
        // replaying
        trx->set_depends_seqno(trx->global_seqno() - 1);
        ApplyOrder ao(*trx);
        if (apply_monitor_.entered(ao) == false)
        {
            gu_trace(apply_monitor_.enter(ao));
        }
        trx->set_state(TrxHandle::S_MUST_REPLAY_CM);
    }
    // fall through
    case TrxHandle::S_MUST_REPLAY_CM:
        if (co_mode_ != CommitOrder::BYPASS)
        {
            CommitOrder co(*trx, co_mode_);
            if (commit_monitor_.entered(co) == false)
            {
                gu_trace(commit_monitor_.enter(co));
            }
        }
        trx->set_state(TrxHandle::S_MUST_REPLAY);
        // fall through
    case TrxHandle::S_MUST_REPLAY:
        ++local_replays_;

        trx->set_state(TrxHandle::S_REPLAYING);
        try
        {
            // Only committing transactions should be replayed
            assert(trx->flags() & TrxHandle::F_COMMIT);

            wsrep_trx_meta_t meta = {{ state_uuid_,     trx->global_seqno() },
                                     { trx->source_id(), trx->trx_id(),
                                       trx->conn_id()                       },
                                     trx->depends_seqno()};

            /* failure to replay own trx is certainly a sign of inconsistency,
             * not trying to catch anything here */
            gu_trace(trx->apply(trx_ctx, apply_cb_, meta));

            uint32_t const commit_flags
                (TrxHandle::trx_flags_to_wsrep_flags(trx->flags()));
            wsrep_bool_t unused(false);
            wsrep_cb_status_t const rcode
                (commit_cb_(trx_ctx, NULL, commit_flags, &meta, &unused));

            if (gu_unlikely(rcode != WSREP_CB_SUCCESS))
                gu_throw_fatal << (commit_flags & WSREP_FLAG_ROLLBACK ?
                                   "Rollback" : "Commit")
                               << " failed. Trx: " << *trx;

            log_debug << "replayed " << trx->global_seqno();
            // trx, ts states will be set to COMMITTED in post_commit()
        }
        catch (gu::Exception& e)
        {
            log_fatal << "Failed to re-apply trx: " << *trx;
            log_fatal << e.what();
            log_fatal << "Node consistency compromized, aborting...";

            mark_corrupt_and_close();
            throw;

#ifdef PERCONA
            /* Ideally this shouldn't fail but if it does then we need
            to ensure clean shutdown with termination of all mysql threads
            and galera replication and rollback threads.
            Currently wsrep part of the code just invokes unireg_abort
            which doesn't ensure this clean shutdown.
            So for now we take the same approach like we do with normal
            apply transaction failure. */
            log_fatal << "Failed to re-apply trx: " << *trx;
            log_fatal << e.what();
            log_fatal << "Node consistency compromized, aborting...";

            /* Before doing a graceful exit ensure that node isolate itself
            from the cluster. This will cause the quorum to re-evaluate
            and if minority nodes are left with different set of data
            they can turn non-Primary to avoid further data consistency issue. */
            param_set("gmcast.isolate", "1");

            abort();
#endif
        }

        // apply, commit monitors are released in post commit
        return WSREP_OK;
    default:
        gu_throw_fatal << "Invalid state in replay for trx " << *trx;
    }

    log_debug << "replaying failed for trx " << *trx;
    assert(trx->state() == TrxHandle::S_ABORTING);

    return retval;
}


wsrep_status_t galera::ReplicatorSMM::post_rollback(TrxHandle* trx)
{
    // * Cert failure or BF abort while inside pre_commit() call or
    // * BF abort between pre_commit() and pre_rollback() call
    assert(trx->state() == TrxHandle::S_EXECUTING ||
           trx->state() == TrxHandle::S_ABORTING  ||
           trx->state() == TrxHandle::S_MUST_ABORT);

    if (trx->state() == TrxHandle::S_MUST_ABORT)
    {
        trx->set_state(TrxHandle::S_ABORTING);
    }

    if (trx->pa_unsafe())
    {
        ApplyOrder ao(*trx);
        apply_monitor_.enter(ao);
    }

    if (co_mode_ != CommitOrder::BYPASS)
    {
        CommitOrder co(*trx, co_mode_);
        commit_monitor_.enter(co);
    }

    return WSREP_OK;
}


wsrep_status_t galera::ReplicatorSMM::interim_commit(TrxHandle* trx)
{
    log_debug << "interim_commit() for trx: " << *trx;

    assert((trx->flags() & TrxHandle::F_ROLLBACK) == 0);

    if (trx->state() == TrxHandle::S_MUST_ABORT)
    {
        // This is possible in case of ALG: BF applier BF aborts
        // trx that has already grabbed commit monitor and is committing.
        // However, this should be acceptable assuming that commit
        // operation does not reserve any more resources and is able
        // to release already reserved resources.
        log_debug << "trx was BF aborted during commit: " << *trx;
        // manipulate state to avoid crash
        trx->set_state(TrxHandle::S_MUST_REPLAY);
        trx->set_state(TrxHandle::S_REPLAYING);
    }
    assert(trx->state() == TrxHandle::S_COMMITTING ||
           trx->state() == TrxHandle::S_REPLAYING);
    assert(trx->local_seqno() > -1 && trx->global_seqno() > -1);

    CommitOrder co(*trx, co_mode_);
    if (co_mode_ != CommitOrder::BYPASS)
    {
        commit_monitor_.leave(co);

        // Allow tests to block the applier thread using the DBUG facilities
        GU_DBUG_SYNC_WAIT("sync.interim_commit.after_commit_leave");
    }
    trx->mark_interim_committed(true);

    return WSREP_OK;
}


wsrep_status_t galera::ReplicatorSMM::release_commit(TrxHandle* trx)
{
    log_debug << "release_commit() for trx: " << *trx;

    if (trx->state() == TrxHandle::S_MUST_ABORT)
    {
        // This is possible in case of ALG: BF applier BF aborts
        // trx that has already grabbed commit monitor and is committing.
        // However, this should be acceptable assuming that commit
        // operation does not reserve any more resources and is able
        // to release already reserved resources.
        log_debug << "trx was BF aborted during commit: " << *trx;
        // manipulate state to avoid crash
        trx->set_state(TrxHandle::S_MUST_REPLAY);
        trx->set_state(TrxHandle::S_REPLAYING);
    }
    assert(trx->state() == TrxHandle::S_COMMITTING ||
           trx->state() == TrxHandle::S_REPLAYING);
    assert(trx->local_seqno() > -1 && trx->global_seqno() > -1);

    if (!(trx->is_interim_committed()))
    {
        CommitOrder co(*trx, co_mode_);
        if (co_mode_ != CommitOrder::BYPASS) commit_monitor_.leave(co);

        // Allow tests to block the applier thread using the DBUG facilities
        GU_DBUG_SYNC_WAIT("sync.post_commit.after_commit_leave");
    }
    trx->mark_interim_committed(false);

    trx->set_state(TrxHandle::S_COMMITTED);

    wsrep_seqno_t const safe_to_discard(cert_.set_trx_committed(*trx));

    ApplyOrder ao(*trx);
    apply_monitor_.leave(ao);

    ++local_commits_;

    report_last_committed(safe_to_discard);

    return WSREP_OK;
}


wsrep_status_t galera::ReplicatorSMM::release_rollback(TrxHandle* trx)
{
    if (trx->state() == TrxHandle::S_MUST_ABORT) // BF abort before replicaiton
        trx->set_state(TrxHandle::S_ABORTING);

    log_debug << "release_rollback() trx: " << *trx;

    if (trx->write_set_in().size() > 0)
    {
        assert(trx->global_seqno() > 0); // BF'ed
        assert(trx->state() == TrxHandle::S_ABORTING);

        log_debug << "Master rolled back " << trx->global_seqno();

        report_last_committed(cert_.set_trx_committed(*trx));

        if (co_mode_ != CommitOrder::BYPASS)
        {
            CommitOrder co(*trx, co_mode_);
            assert(commit_monitor_.entered(co));
            commit_monitor_.leave(co);
        }

        ApplyOrder ao(*trx);
        if (apply_monitor_.entered(ao)) apply_monitor_.leave(ao);
    }

    assert(trx->state() == TrxHandle::S_ABORTING ||
           trx->state() == TrxHandle::S_EXECUTING);

    trx->set_state(TrxHandle::S_ROLLED_BACK);

    // Trx was either rolled back by user or via certification failure,
    // last committed report not needed since cert index state didn't change.
    // report_last_committed();
    ++local_rollbacks_;

    return WSREP_OK;
}


wsrep_status_t galera::ReplicatorSMM::sync_wait(wsrep_gtid_t* upto,
                                                int           tout,
                                                wsrep_gtid_t* gtid)
{
    gu::GTID wait_gtid;

    if (upto == 0)
    {
        long ret = gcs_.caused(wait_gtid);
        if (ret < 0)
        {
            log_warn << "gcs_caused() returned " << ret
                     << " ("  << strerror(-ret) << ')';
            return WSREP_TRX_FAIL;
        }
    }
    else
    {
        wait_gtid.set(upto->uuid, upto->seqno);
    }

    try
    {
        // @note: Using timed wait for monitor is currently a hack
        // to avoid deadlock resulting from race between monitor wait
        // and drain during configuration change. Instead of this,
        // monitor should have proper mechanism to interrupt waiters
        // at monitor drain and disallowing further waits until
        // configuration change related operations (SST etc) have been
        // finished.
        gu::datetime::Period timeout(causal_read_timeout_);
        if (tout != -1)
        {
            timeout = gu::datetime::Period(tout * gu::datetime::Sec);
        }
        gu::datetime::Date wait_until(gu::datetime::Date::calendar() + timeout);

#ifdef GALERA
        if (gu_likely(co_mode_ != CommitOrder::BYPASS))
        {
            commit_monitor_.wait(wait_gtid, wait_until);
        }
        else
#endif
        // PXC release commit monitor before the real commit is done (and rely on
        // MySQL group commit protocol).
        // This means sync-wait based on commit ordering may get wrong result.
        {
            apply_monitor_.wait(wait_gtid, wait_until);
        }
        if (gtid != 0)
        {
            apply_monitor_.last_left_gtid(*gtid);
        }
        ++causal_reads_;
        return WSREP_OK;
    }
    catch (gu::NotFound& e)
    {
        log_debug << "monitor wait failed for sync_wait: UUID mismatch";
        return WSREP_TRX_MISSING;
    }
    catch (gu::Exception& e)
    {
        log_info << "monitor wait failed for sync wait (repl.causal_read_timeout): " << e.what();
        return WSREP_TRX_FAIL;
    }
}

wsrep_status_t galera::ReplicatorSMM::last_committed_id(wsrep_gtid_t* gtid)
{
    commit_monitor_.last_left_gtid(*gtid);
    return WSREP_OK;
}

wsrep_status_t galera::ReplicatorSMM::to_isolation_begin(TrxHandlePtr&     txp,
                                                         wsrep_trx_meta_t* meta)
{
    TrxHandle& trx(*txp);

    if (meta != 0)
    {
        assert(meta->gtid.seqno > 0);
        assert(meta->gtid.seqno == trx.global_seqno());
        assert(meta->depends_on == trx.depends_seqno());
    }

    assert(trx.state() == TrxHandle::S_REPLICATING);
    trx.set_state(TrxHandle::S_CERTIFYING);

    assert(trx.trx_id() == static_cast<wsrep_trx_id_t>(-1));
    assert(trx.local_seqno() > -1 && trx.global_seqno() > -1);
    assert(trx.global_seqno() > STATE_SEQNO());

    CommitOrder co(trx, co_mode_);
    wsrep_status_t retval;
    switch ((retval = cert_and_catch(txp)))
    {
    case WSREP_OK:
    {
        ApplyOrder ao(trx);
        gu_trace(apply_monitor_.enter(ao));

        trx.set_state(TrxHandle::S_APPLYING);
        trx.set_state(TrxHandle::S_COMMITTING);
        break;
    }
    case WSREP_TRX_FAIL:
        // Apply monitor is released in cert() in case of failure.
        trx.set_state(TrxHandle::S_ABORTING);
        break;
    default:
        assert(0);
        gu_throw_fatal << "unrecognized retval " << retval
                       << " for to isolation certification for " << trx;
        break;
    }

    if (co_mode_ != CommitOrder::BYPASS)
        try
        {
            commit_monitor_.enter(co);

            if (trx.state() == TrxHandle::S_COMMITTING)
            {
                log_debug << "Executing TO isolated action: " << trx;
                st_.mark_unsafe();
            }
            else
            {
                log_debug << "Grabbed TO for failed isolated action: " << trx;
                assert(trx.state() == TrxHandle::S_ABORTING);
            }
        }
        catch (...)
        {
            gu_throw_fatal << "unable to enter commit monitor: " << trx;
        }

    return retval;
}


wsrep_status_t
galera::ReplicatorSMM::to_isolation_end(TrxHandlePtr& txp,
                                        const wsrep_buf_t* const err)
{
    TrxHandle& trx(*txp);

    log_debug << "Done executing TO isolated action: " << trx
              << ", error message: " << gu::Hexdump(err->ptr, err->len, true);

    assert(trx.state() == TrxHandle::S_COMMITTING ||
           trx.state() == TrxHandle::S_ABORTING);

    CommitOrder co(trx, co_mode_);
    if (co_mode_ != CommitOrder::BYPASS)
    {
        commit_monitor_.leave(co);
        GU_DBUG_SYNC_WAIT("sync.to_isolation_end.after_commit_leave");
    }
    ApplyOrder ao(trx);
    report_last_committed(cert_.set_trx_committed(trx));
    apply_monitor_.leave(ao);

    st_.mark_safe();

    if (trx.state() == TrxHandle::S_COMMITTING)
    {
        assert(trx.state() == TrxHandle::S_COMMITTING);
        trx.set_state(TrxHandle::S_COMMITTED);
    }
    else
    {
        // apply_monitor_ was canceled in cert()
        assert(trx.state() == TrxHandle::S_ABORTING);
        trx.set_state(TrxHandle::S_ROLLED_BACK);
    }

    return WSREP_OK;
}

namespace galera
{

static WriteSetOut*
writeset_from_handle (wsrep_po_handle_t& handle,
                      const TrxHandle::Params& trx_params)
{
    WriteSetOut* ret = static_cast<WriteSetOut*>(handle.opaque);

    if (NULL == ret)
    {
        try
        {
            ret = new WriteSetOut(
//                gu::String<256>(trx_params.working_dir_) << '/' << &handle,
                trx_params.working_dir_, wsrep_trx_id_t(&handle),
                /* key format is not essential since we're not adding keys */
                KeySet::version(trx_params.key_format_), NULL, 0, 0,
                trx_params.record_set_ver_,
                WriteSetNG::MAX_VERSION, DataSet::MAX_VERSION, DataSet::MAX_VERSION,
                trx_params.max_write_set_size_);

            handle.opaque = ret;
        }
        catch (std::bad_alloc& ba)
        {
            gu_throw_error(ENOMEM) << "Could not create WriteSetOut";
        }
    }

    return ret;
}

} /* namespace galera */

wsrep_status_t
galera::ReplicatorSMM::preordered_collect(wsrep_po_handle_t&            handle,
                                          const struct wsrep_buf* const data,
                                          size_t                  const count,
                                          bool                    const copy)
{
    WriteSetOut* const ws(writeset_from_handle(handle, trx_params_));

    for (size_t i(0); i < count; ++i)
    {
        ws->append_data(data[i].ptr, data[i].len, copy);
    }

    return WSREP_OK;
}


wsrep_status_t
galera::ReplicatorSMM::preordered_commit(wsrep_po_handle_t&            handle,
                                         const wsrep_uuid_t&           source,
                                         uint64_t                const flags,
                                         int                     const pa_range,
                                         bool                    const commit)
{
    WriteSetOut* const ws(writeset_from_handle(handle, trx_params_));

    if (gu_likely(true == commit))
    {
        ws->set_flags (WriteSetNG::wsrep_flags_to_ws_flags(flags));

        /* by loooking at trx_id we should be able to detect gaps / lost events
         * (however resending is not implemented yet). Something like
         *
         * wsrep_trx_id_t const trx_id(cert_.append_preordered(source, ws));
         *
         * begs to be here. */
        wsrep_trx_id_t const trx_id(preordered_id_.add_and_fetch(1));

        WriteSetNG::GatherVector actv;

        size_t const actv_size(ws->gather(source, 0, trx_id, actv));

        ws->finalize_preordered (pa_range); // also adds CRC

        int rcode;
        do
        {
            rcode = gcs_.sendv(actv, actv_size, GCS_ACT_TORDERED, false, false);
        }
        while (rcode == -EAGAIN && (usleep(1000), true));

        if (rcode < 0)
            gu_throw_error(-rcode)
                << "Replication of preordered writeset failed.";
    }

    delete ws;

    handle.opaque = NULL;

    return WSREP_OK;
}


wsrep_status_t
galera::ReplicatorSMM::sst_sent(const wsrep_gtid_t& state_id, int const rcode)
{
    assert (rcode <= 0);
    assert (rcode == 0 || state_id.seqno == WSREP_SEQNO_UNDEFINED);
    assert (rcode != 0 || state_id.seqno >= 0);

    GU_DBUG_SYNC_WAIT("sst_sent");

    if (state_() != S_DONOR)
    {
        log_error << "sst sent called when not SST donor, state " << state_();
        /* If sst-sent fails node should restore itself back to joined state.
        sst-sent can fail commonly due to n/w error where-in DONOR may loose
        connectivity to JOINER (or existing cluster) but on re-join it should
        restore the original state (DONOR->JOINER->JOINED->SYNCED) without
        waiting for JOINER. sst-failure on JOINER will gracefully shutdown the
        joiner. */
        gcs_.join_notification();
        return WSREP_CONN_FAIL;
    }

    gcs_seqno_t seqno(rcode ? rcode : state_id.seqno);

    if (state_id.uuid != state_uuid_ && seqno >= 0)
    {
        // state we have sent no longer corresponds to the current group state
        // mark an error
        seqno = -EREMCHG;
    }

    try {
        if (rcode == 0)
            gcs_.join(gu::GTID(state_id.uuid, state_id.seqno), rcode);
        else
            /* stamp error message with the current state */
            gcs_.join(gu::GTID(state_uuid_, commit_monitor_.last_left()), rcode);

        return WSREP_OK;
    }
    catch (gu::Exception& e)
    {
        log_error << "failed to recover from DONOR state: " << e.what();
        return WSREP_CONN_FAIL;
    }
}


void galera::ReplicatorSMM::process_trx(void* recv_ctx, const TrxHandlePtr& trx)
{
    assert(recv_ctx != 0);
    assert(trx != 0);
    assert(trx->local_seqno() > 0);
    assert(trx->global_seqno() > 0);
    assert(trx->last_seen_seqno() >= 0);
    assert(trx->depends_seqno() == -1);
    assert(trx->state() == TrxHandle::S_REPLICATING);

    // If the SST has been canceled, then ignore any other
    // incoming transactions, as the node should be shutting down
    if (sst_state_ == SST_CANCELED)
    {
        log_info << "Ignorng trx(" << trx->global_seqno() << ") due to SST failure";
        return;
    }

    trx->set_state(TrxHandle::S_CERTIFYING);

    wsrep_status_t const retval(cert_and_catch(trx));
    switch (retval)
    {
    case WSREP_TRX_FAIL:
        assert(trx->state() == TrxHandle::S_ABORTING);
        /* fall through to apply_trx() */
    case WSREP_OK:
        try
        {
            gu_trace(apply_trx(recv_ctx, *trx));
        }
        catch (std::exception& e)
        {
            log_fatal << "Failed to apply trx: " << *trx;
            log_fatal << e.what();
            log_fatal << "Node consistency compromized, leaving cluster...";

#ifdef PERCONA
            /* Before doing a graceful exit ensure that node isolate itself
            from the cluster. This will cause the quorum to re-evaluate
            and if minority nodes are left with different set of data
            they can turn non-Primary to avoid further data consistency issue. */
            param_set("gmcast.isolate", "1");
#endif

            mark_corrupt_and_close();
            assert(0); // this is an unexpected exception
            // keep processing events from the queue until provider is closed
        }
        break;
    case WSREP_TRX_MISSING: // must be skipped due to SST
        assert(trx->state() == TrxHandle::S_ABORTING);
        break;
    default:
        // this should not happen for remote actions
        gu_throw_error(EINVAL)
            << "unrecognized retval for remote trx certification: "
            << retval << " trx: " << *trx;
    }
}


void galera::ReplicatorSMM::process_commit_cut(wsrep_seqno_t seq,
                                               wsrep_seqno_t seqno_l)
{
    assert(seq > 0);
    assert(seqno_l > 0);

    LocalOrder lo(seqno_l);

    gu_trace(local_monitor_.enter(lo));

    if (seq >= cc_seqno_) /* Refs #782. workaround for
                           * assert(seqno >= seqno_released_) in gcache. */
        cert_.purge_trxs_upto(seq, true);

    local_monitor_.leave(lo);
    log_debug << "Got commit cut from GCS: " << seq;
}


/* NB: the only use for this method is in cancel_seqnos() below */
void galera::ReplicatorSMM::cancel_seqno(wsrep_seqno_t const seqno)
{
    assert(seqno > 0);

    ApplyOrder ao(seqno, seqno - 1);
    apply_monitor_.self_cancel(ao);

    if (co_mode_ != CommitOrder::BYPASS)
    {
        CommitOrder co(seqno, co_mode_);
        commit_monitor_.self_cancel(co);
    }
}

/* NB: the only use for this method is to dismiss the slave queue
 *     in corrupt state */
void galera::ReplicatorSMM::cancel_seqnos(wsrep_seqno_t const seqno_l,
                                          wsrep_seqno_t const seqno_g)
{
    if (seqno_l > 0)
    {
        LocalOrder lo(seqno_l);
        local_monitor_.self_cancel(lo);
    }

    if (seqno_g > 0) cancel_seqno(seqno_g);
}


void galera::ReplicatorSMM::drain_monitors(wsrep_seqno_t const upto)
{
    apply_monitor_.drain(upto);
    if (co_mode_ != CommitOrder::BYPASS) commit_monitor_.drain(upto);
}


void galera::ReplicatorSMM::set_initial_position(const wsrep_uuid_t&  uuid,
                                                 wsrep_seqno_t const seqno)
{
    update_state_uuid(uuid);
    apply_monitor_.set_initial_position(uuid, seqno);

    if (co_mode_ != CommitOrder::BYPASS)
        commit_monitor_.set_initial_position(uuid, seqno);
}

void galera::ReplicatorSMM::establish_protocol_versions (int proto_ver)
{
    trx_params_.record_set_ver_ = gu::RecordSet::VER1;

    switch (proto_ver)
    {
    case 1:
        trx_params_.version_ = 1;
        str_proto_ver_ = 0;
        break;
    case 2:
        trx_params_.version_ = 1;
        str_proto_ver_ = 1;
        break;
    case 3:
    case 4:
        trx_params_.version_ = 2;
        str_proto_ver_ = 1;
        break;
    case 5:
        trx_params_.version_ = 3;
        str_proto_ver_ = 1;
        break;
    case 6:
        trx_params_.version_  = 3;
        str_proto_ver_ = 2; // gcs intelligent donor selection.
        // include handling dangling comma in donor string.
        break;
    case 7:
        // Protocol upgrade to handle IST SSL backwards compatibility,
        // no effect to TRX or STR protocols.
        trx_params_.version_ = 3;
        str_proto_ver_ = 2;
        break;
    case 8:
        // Protocol upgrade to enforce 8-byte alignment in writesets.
        trx_params_.version_ = 3;
        trx_params_.record_set_ver_ = gu::RecordSet::VER2;
        str_proto_ver_ = 2;
        break;
    default:
        log_fatal << "Configuration change resulted in an unsupported protocol "
            "version: " << proto_ver << ". Can't continue.";
        abort();
    };

    protocol_version_ = proto_ver;
    log_info << "REPL Protocols: " << protocol_version_ << " ("
              << trx_params_.version_ << ", " << str_proto_ver_ << ")";
}

static bool
app_wants_state_transfer (const void* const req, ssize_t const req_len)
{
    return (req_len != (strlen(WSREP_STATE_TRANSFER_NONE) + 1) ||
            memcmp(req, WSREP_STATE_TRANSFER_NONE, req_len));
}

void
galera::ReplicatorSMM::update_incoming_list(const wsrep_view_info_t& view)
{
    static char const separator(',');

    ssize_t new_size(0);

    if (view.memb_num > 0)
    {
        new_size += view.memb_num - 1; // separators

        for (int i = 0; i < view.memb_num; ++i)
        {
            new_size += strlen(view.members[i].incoming);
        }
    }

    gu::Lock lock(incoming_mutex_);

    incoming_list_.clear();
    incoming_list_.resize(new_size);

    if (new_size <= 0) return;

    incoming_list_ = view.members[0].incoming;

    for (int i = 1; i < view.memb_num; ++i)
    {
        incoming_list_ += separator;
        incoming_list_ += view.members[i].incoming;
    }
}

static galera::Replicator::State state2repl(gcs_node_state const my_state,
                                            int const            my_idx)
{
    switch (my_state)
    {
    case GCS_NODE_STATE_NON_PRIM:
    case GCS_NODE_STATE_PRIM:
        return galera::Replicator::S_CONNECTED;
    case GCS_NODE_STATE_JOINER:
        return galera::Replicator::S_JOINING;
    case GCS_NODE_STATE_JOINED:
        return galera::Replicator::S_JOINED;
    case GCS_NODE_STATE_SYNCED:
        return galera::Replicator::S_SYNCED;
    case GCS_NODE_STATE_DONOR:
        return galera::Replicator::S_DONOR;
    case GCS_NODE_STATE_MAX:
        assert(0);
    }

    gu_throw_fatal << "unhandled gcs state: " << my_state;
    GU_DEBUG_NORETURN;
}

void
galera::ReplicatorSMM::process_conf_change(void*                    recv_ctx,
                                           const struct gcs_action& act)
{
    assert(act.seqno_l >= 0);

    // If SST operation was canceled, we shall immediately
    // return from the function to avoid hang-up in the monitor
    // drain code and avoid restart of the SST.
    if (sst_state_ == SST_CANCELED)
    {
        // We must resume receiving messages from gcs.
        gcs_.resume_recv();
        return;
    }

    LocalOrder lo(act.seqno_l);
    gu_trace(local_monitor_.enter(lo));

    const gcs_act_conf_t& conf(*static_cast<const gcs_act_conf_t*>(act.buf));

    log_debug << "Received CC action: seqno_g: " << act.seqno_g
              << ", seqno_l: " << act.seqno_l << ", conf(id: " << conf.conf_id
              << ", memb_num: " << conf.memb_num << ", my_idx: " << conf.my_idx
              << ", my_state: " << conf.my_state << ", seqno: " << conf.seqno
              << ")";

    wsrep_view_info_t* const view_info
        (galera_view_info_create(&conf, conf.my_state == GCS_NODE_STATE_PRIM));

    assert(view_info->memb_num == conf.memb_num);
    assert(view_info->my_idx == conf.my_idx);
    assert(view_info->view < 0 || view_info->my_idx >= 0);

    update_incoming_list(*view_info);

    wsrep_seqno_t const upto(cert_.position());
    gu_trace(drain_monitors(upto));

    if (view_info->status == WSREP_VIEW_PRIMARY)
    {
        safe_to_bootstrap_ = (view_info->memb_num == 1);
    }

    wsrep_seqno_t const group_seqno(view_info->state_id.seqno);
    const wsrep_uuid_t& group_uuid (view_info->state_id.uuid);

    if (view_info->my_idx >= 0)
    {
        bool const first_view(uuid_ == WSREP_UUID_UNDEFINED);
        if (first_view) uuid_ = view_info->members[view_info->my_idx].id;

        // First view from the group or group uuid has changed,
        // call connected callback to notify application.
        if ((first_view || state_uuid_ != group_uuid) && connected_cb_)
        {
            wsrep_cb_status_t cret(connected_cb_(0, view_info));
            if (cret != WSREP_CB_SUCCESS)
            {
                log_fatal << "Application returned error " << cret
                          << " from connect callback, aborting";
                abort();
            }
        }
    }

    bool const st_required
        (state_transfer_required(*view_info, conf.my_state == GCS_NODE_STATE_PRIM));

    void*  app_req(0);
    size_t app_req_len(0);

    if (st_required)
    {
        log_info << "State transfer required: "
                 << "\n\tGroup state: " << group_uuid << ":" << group_seqno
                 << "\n\tLocal state: " << state_uuid_<< ":" << STATE_SEQNO();

        if (S_CONNECTED != state_()) state_.shift_to(S_CONNECTED);

        wsrep_cb_status_t const rcode(sst_request_cb_(&app_req, &app_req_len));

        if (WSREP_CB_SUCCESS != rcode)
        {
            assert(app_req_len <= 0);
            log_fatal << "SST request callback failed. This is unrecoverable, "
                      << "restart required.";
            abort();
        }
        else if (0 == app_req_len && state_uuid_ != group_uuid)
        {
            log_fatal << "Local state UUID " << state_uuid_
                      << " is different from group state UUID " << group_uuid
                      << ", and SST request is null: restart required.";
            abort();
        }
    }

    Replicator::State const next_state(state2repl(conf.my_state, conf.my_idx));

    if (view_info->view >= 0) // Primary configuration
    {
        establish_protocol_versions(conf.repl_proto_ver);

        // we have to reset cert initial position here, SST does not contain
        // cert index yet (see #197).
        cert_.assign_initial_position(gu::GTID(group_uuid, group_seqno),
                                      trx_params_.version_);
        // at this point there is no ongoing master or slave transactions
        // and no new requests to service thread should be possible

        if (STATE_SEQNO() > 0) service_thd_.release_seqno(STATE_SEQNO());
        // make sure all gcache buffers are released

        service_thd_.flush(group_uuid); // make sure service thd is idle

        // record state seqno, needed for IST on DONOR
        cc_seqno_ = group_seqno;

        bool const app_wants_st(app_wants_state_transfer(app_req, app_req_len));

        if (st_required && app_wants_st)
        {
            // GCache::Seqno_reset() happens here
            long ret =
            request_state_transfer (recv_ctx,
                                    group_uuid, group_seqno, app_req,
                                    app_req_len);

            if (ret < 0 || sst_state_ == SST_CANCELED)
            {
                // If the IST/SST request was canceled due to error
                // at the GCS level or if request was canceled by another
                // thread (by initiative of the server), and if the node
                // remain in the S_JOINING state, then we must return it
                // to the S_CONNECTED state (to the original state, which
                // exist before the request_state_transfer started).
                // In other words, if state transfer failed, then we
                // need to move node back to the original state, because
                // joining was canceled:

                if (state_() == S_JOINING)
                {
                    state_.shift_to(S_CONNECTED);
                }
            }
        }
        else
        {
            if (view_info->view == 1 || !app_wants_st)
            {
                set_initial_position(group_uuid, group_seqno);
                gcache_.seqno_reset(gu::GTID(group_uuid, group_seqno));
            }

            if (state_() == S_CONNECTED || state_() == S_DONOR)
            {
                switch (next_state)
                {
                case S_JOINING:
                    state_.shift_to(S_JOINING);
                    break;
                case S_DONOR:
                    if (state_() == S_CONNECTED)
                    {
                        state_.shift_to(S_DONOR);
                    }
                    break;
                case S_JOINED:
                    state_.shift_to(S_JOINED);
                    break;
                case S_SYNCED:
                    state_.shift_to(S_SYNCED);
                    synced_cb_(app_ctx_);
                    break;
                default:
                    log_debug << "next_state " << next_state;
                    break;
                }
            }

            st_.set(state_uuid_, WSREP_SEQNO_UNDEFINED, safe_to_bootstrap_);
        }

        // We should not try to joining the cluster at the GCS level,
        // if the node is not in the S_JOINING state, or if we did not
        // sent the IST/SST request, or if it is failed. In other words,
        // any state other than SST_WAIT (f.e. SST_NONE or SST_CANCELED)
        // not require us to sending the JOIN message at the GCS level:

        if (sst_state_ == SST_WAIT && state_() == S_JOINING)
#ifdef GALERA
        if (state_() == S_JOINING && sst_state_ != SST_NONE)
#endif
        {
            /* There are two reasons we can be here:
             * 1) we just got state transfer in request_state_transfer() above;
             * 2) we failed here previously (probably due to partition).
             */
            try {
                gcs_.join(gu::GTID(state_uuid_, sst_seqno_), 0);
                sst_state_ = SST_NONE;
            }
            catch (gu::Exception& e)
            {
                log_error << "Failed to JOIN the cluster after SST";
            }
        }
    }
    else
    {
        // Non-primary configuration
        if (state_uuid_ != WSREP_UUID_UNDEFINED)
        {
            st_.set (state_uuid_, STATE_SEQNO(), safe_to_bootstrap_);
        }

        if (next_state != S_CONNECTED)
        {
            log_fatal << "Internal error: unexpected next state for "
                      << "non-prim: " << next_state
                      << ". Current state: " << state_() <<". Restart required.";
            local_monitor_.leave(lo);
            close();
            abort();
        }

        state_.shift_to(next_state);
    }

    {
        wsrep_cb_status_t const rcode
            (view_cb_(app_ctx_, recv_ctx, view_info, 0, 0));

        if (WSREP_CB_SUCCESS != rcode)
        {
            assert(app_req_len <= 0);
            log_fatal << "View callback failed. This is unrecoverable, "
                      << "restart required.";
            abort();
        }
    }

    local_monitor_.leave(lo);
    // Wake up potential IST waters
    gcs_.resume_recv();
    ist_end(0);

    free(app_req);
    free(view_info);

    if (conf.conf_id < 0 && conf.memb_num == 0) {
        log_debug << "Received SELF-LEAVE. Connection closed.";

        gu::Lock lock(closing_mutex_);

        shift_to_CLOSED();
    }
}


void galera::ReplicatorSMM::process_join(wsrep_seqno_t seqno_j,
                                         wsrep_seqno_t seqno_l)
{
    LocalOrder lo(seqno_l);

    gu_trace(local_monitor_.enter(lo));

    wsrep_seqno_t const upto(cert_.position());

    apply_monitor_.drain(upto);

    if (co_mode_ != CommitOrder::BYPASS) commit_monitor_.drain(upto);

    if (seqno_j < 0 && S_JOINING == state_())
    {
        // #595, @todo: find a way to re-request state transfer
        log_fatal << "Failed to receive state transfer: " << seqno_j
                  << " (" << strerror (-seqno_j) << "), need to restart.";
        abort();
    }
    else
    {
        state_.shift_to(S_JOINED);
    }

    local_monitor_.leave(lo);
}


void galera::ReplicatorSMM::process_sync(wsrep_seqno_t seqno_l)
{
    LocalOrder lo(seqno_l);

    gu_trace(local_monitor_.enter(lo));

    wsrep_seqno_t const upto(cert_.position());

    apply_monitor_.drain(upto);

    if (co_mode_ != CommitOrder::BYPASS) commit_monitor_.drain(upto);

    state_.shift_to(S_SYNCED);
    synced_cb_(app_ctx_);
    local_monitor_.leave(lo);
}

wsrep_seqno_t galera::ReplicatorSMM::pause()
{
    // Grab local seqno for local_monitor_
    wsrep_seqno_t const local_seqno(
        static_cast<wsrep_seqno_t>(gcs_.local_sequence()));
    LocalOrder lo(local_seqno);
    local_monitor_.enter(lo);

    // Local monitor should take care that concurrent
    // pause requests are enqueued
    assert(pause_seqno_ == WSREP_SEQNO_UNDEFINED);
    pause_seqno_ = local_seqno;

    // Get drain seqno from cert index
    wsrep_seqno_t const upto(cert_.position());
    apply_monitor_.drain(upto);
    assert (apply_monitor_.last_left() >= upto);

    if (co_mode_ != CommitOrder::BYPASS)
    {
        commit_monitor_.drain(upto);
        assert (commit_monitor_.last_left() >= upto);
        assert (commit_monitor_.last_left() == apply_monitor_.last_left());
    }

    wsrep_seqno_t const ret(STATE_SEQNO());
    st_.set(state_uuid_, ret, safe_to_bootstrap_);

    log_info << "Provider paused at " << state_uuid_ << ':' << ret
             << " (" << pause_seqno_ << ")";

    return ret;
}

void galera::ReplicatorSMM::resume()
{
    if (pause_seqno_ == WSREP_SEQNO_UNDEFINED)
    {
        log_warn << "tried to resume unpaused provider";
        return;
    }

    st_.set(state_uuid_, WSREP_SEQNO_UNDEFINED, safe_to_bootstrap_);
    log_info << "resuming provider at " << pause_seqno_;
    LocalOrder lo(pause_seqno_);
    pause_seqno_ = WSREP_SEQNO_UNDEFINED;
    local_monitor_.leave(lo);
    log_info << "Provider resumed.";
}

void galera::ReplicatorSMM::desync()
{
    wsrep_seqno_t seqno_l;

    ssize_t const ret(gcs_.desync(seqno_l));

    if (seqno_l > 0)
    {
        LocalOrder lo(seqno_l); // need to process it regardless of ret value

        if (ret == 0)
        {
/* #706 - the check below must be state request-specific. We are not holding
          any locks here and must be able to wait like any other action.
          However practice may prove different, leaving it here as a reminder.
            if (local_monitor_.would_block(seqno_l))
            {
                gu_throw_error (-EDEADLK) << "Ran out of resources waiting to "
                                          << "desync the node. "
                                          << "The node must be restarted.";
            }
*/
            local_monitor_.enter(lo);
            if (state_() != S_DONOR) state_.shift_to(S_DONOR);
            local_monitor_.leave(lo);
            GU_DBUG_SYNC_WAIT("wsrep_desync_left_local_monitor");
        }
        else if (ret != -EAGAIN)
        {
            local_monitor_.self_cancel(lo);
        }
    }

    if (ret)
    {
        gu_throw_error (-ret) << "Node desync failed.";
    }
}

void galera::ReplicatorSMM::resync()
{
    gcs_.join(gu::GTID(state_uuid_, commit_monitor_.last_left()), 0);
}


//////////////////////////////////////////////////////////////////////
//////////////////////////////////////////////////////////////////////
////                           Private
//////////////////////////////////////////////////////////////////////
//////////////////////////////////////////////////////////////////////

/* don't use this directly, use cert_and_catch() instead */
inline
wsrep_status_t galera::ReplicatorSMM::cert(const TrxHandlePtr& trx)
{
    assert(trx->state() == TrxHandle::S_CERTIFYING ||
           trx->state() == TrxHandle::S_MUST_CERT_AND_REPLAY);

    assert(trx->local_seqno()     != WSREP_SEQNO_UNDEFINED);
    assert(trx->global_seqno()    != WSREP_SEQNO_UNDEFINED);
    assert(trx->last_seen_seqno() >= 0);
    assert(trx->last_seen_seqno() < trx->global_seqno());

    LocalOrder  lo(*trx);
    bool        interrupted(false);
    bool        in_replay(trx->state() == TrxHandle::S_MUST_CERT_AND_REPLAY);

    try
    {
        trx->unlock();
        if (in_replay == false || local_monitor_.entered(lo) == false)
        {
            gu_trace(local_monitor_.enter(lo));
        }
        trx->lock();
        assert(trx->state() == TrxHandle::S_CERTIFYING ||
               trx->state() == TrxHandle::S_MUST_ABORT ||
               trx->state() == TrxHandle::S_MUST_CERT_AND_REPLAY);
    }
    catch (gu::Exception& e)
    {
        trx->lock();
        if (e.get_errno() == EINTR) { interrupted = true; }
        else throw;
    }

    wsrep_status_t retval(WSREP_OK);
    bool const applicable(trx->global_seqno() > STATE_SEQNO());
    assert(!trx->is_local() || applicable); // applicable can't be false for locals

    if (gu_unlikely(interrupted || trx->state() == TrxHandle::S_MUST_ABORT))
    {
        assert(trx->state() == TrxHandle::S_MUST_ABORT);
        retval = cert_for_aborted(trx);

        if (WSREP_TRX_FAIL == retval)
        {
            assert(WSREP_SEQNO_UNDEFINED == trx->depends_seqno());
            assert(TrxHandle::S_ABORTING == trx->state());

            if (interrupted == true)
            {
                local_monitor_.self_cancel(lo);
            }
            else
            {
                local_monitor_.leave(lo);
            }

            assert(trx->is_dummy());
        }
        else
        {
            assert(WSREP_BF_ABORT == retval);
            assert(trx->flags() & TrxHandle::F_COMMIT);

            trx->set_state(TrxHandle::S_MUST_CERT_AND_REPLAY);
            return retval;
        }
    }
    else
    {
        assert(trx->state() == TrxHandle::S_CERTIFYING ||
               trx->state() == TrxHandle::S_MUST_CERT_AND_REPLAY);

        switch (cert_.append_trx(trx))
        {
        case Certification::TEST_OK:
            if (gu_likely(applicable))
            {
                retval = WSREP_OK;
                assert(trx->depends_seqno() >= 0);
            }
            else
            {
                // this can happen after SST position has been submitted
                // but not all actions preceding SST initial position
                // have been processed
                trx->set_state(TrxHandle::S_ABORTING);
                retval = WSREP_TRX_MISSING;
            }
            break;
        case Certification::TEST_FAILED:
#if GALERA
            if (gu_unlikely(trx->is_toi() && applicable)) // small sanity check
            {
                // In some rare scenarios (e.g., when we have multiple
                // transactions awaiting certification, and the last
                // node remaining in the cluster becomes PRIMARY due
                // to the failure of the previous primary node and
                // the assign_initial_position() was called), sequence
                // number mismatch occurs on configuration change and
                // then certification was failed. We cannot move server
                // forward (with last_seen_seqno < initial_position,
                // see galera::Certification::do_test() for details)
                // to avoid potential data loss, and hence will have
                // to shut it down. Before shutting it down, we need
                // to mark state as unsafe to trigger SST at next
                // server restart.
                log_fatal << "Certification failed for TO isolated action: "
                          << *trx;
                st_.mark_unsafe();
                local_monitor_.leave(lo);
                abort();
            }
#endif
            /* Code above fails to handle TOI (read DDL) transaction
            as DDL are non-atomic and so can't be rolled back in case of
            certification failure. But given the TOI flow, certification
            checks are done well before the real-action starts and so
            error returned at stage shouldn't cause any rollback for TOI/DDL. */
            if (gu_unlikely(trx->is_toi() && applicable))
                log_info << "Certification failed for TO isolated action: "
                          << *trx;
            else
                log_debug << "Certification failed for replicated action: "
                          << *trx;

            local_cert_failures_ += trx->is_local();
            assert(trx->state() == TrxHandle::S_ABORTING);
            retval = applicable ? WSREP_TRX_FAIL : WSREP_TRX_MISSING;
            break;
        }

        // at this point we are about to leave local_monitor_. Make sure
        // trx checksum was alright before that.
        trx->verify_checksum();

        // we must do it 'in order' for std::map reasons, so keeping
        // it inside the monitor
        gcache_.seqno_assign (trx->action(), trx->global_seqno(),
                              GCS_ACT_TORDERED, trx->depends_seqno() < 0);

        if (gu_unlikely(WSREP_TRX_MISSING == retval))
        {
            // the last chance to set trx committed while inside of a monitor
            report_last_committed(cert_.set_trx_committed(*trx));
        }

        local_monitor_.leave(lo);
    }

    assert(WSREP_OK == retval || WSREP_TRX_FAIL == retval ||
           WSREP_TRX_MISSING == retval);

    if (gu_unlikely(WSREP_TRX_FAIL == retval && applicable))
    {
        assert(trx->state() == TrxHandle::S_ABORTING);
        // applicable but failed certification: self-cancel monitors
        cancel_monitors<false>(*trx);
    }
    else
    {
        assert(WSREP_OK != retval || trx->depends_seqno() >= 0);
    }

    return retval;
}

/* pretty much any exception in cert() is fatal as it blocks local_monitor_ */
wsrep_status_t galera::ReplicatorSMM::cert_and_catch(const TrxHandlePtr& trx)
{
    try
    {
        return cert(trx);
    }
    catch (std::exception& e)
    {
        log_fatal << "Certification exception: " << e.what();
    }
    catch (...)
    {
        log_fatal << "Unknown certification exception";
    }
    assert(0);
    abort();
}

/* This must be called BEFORE local_monitor_.self_cancel() due to
 * gcache_.seqno_assign() */
wsrep_status_t galera::ReplicatorSMM::cert_for_aborted(const TrxHandlePtr& trx)
{
    // trx was BF aborted either while it was replicating or
    // while it was waiting for local monitor
    assert(trx->state() == TrxHandle::S_MUST_ABORT);

    Certification::TestResult const res(cert_.test(trx, false));

    switch (res)
    {
    case Certification::TEST_OK:
        return WSREP_BF_ABORT;

    case Certification::TEST_FAILED:
        // Mext step will be monitors release. Make sure that ws was not
        // corrupted and cert failure is real before procedeing with that.
 //gcf788 - this must be moved to cert(), the caller method
        assert(trx->is_dummy());
        trx->verify_checksum();
        gcache_.seqno_assign (trx->action(), trx->global_seqno(),
                              GCS_ACT_TORDERED, true);
        return WSREP_TRX_FAIL;

    default:
        log_fatal << "Unexpected return value from Certification::test(): "
                  << res;
        abort();
    }
}


void
galera::ReplicatorSMM::update_state_uuid (const wsrep_uuid_t& uuid)
{
    if (state_uuid_ != uuid)
    {
        *(const_cast<wsrep_uuid_t*>(&state_uuid_)) = uuid;

        std::ostringstream os; os << state_uuid_;

        strncpy(const_cast<char*>(state_uuid_str_), os.str().c_str(),
                sizeof(state_uuid_str_));
    }

    st_.set(uuid, WSREP_SEQNO_UNDEFINED, safe_to_bootstrap_);
}

void
galera::ReplicatorSMM::abort()
{
#ifdef GALERA
    gcs_.close();
#endif
    close();
    gu_abort();
}
