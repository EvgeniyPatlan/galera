//
// Copyright (C) 2009 Codership Oy <info@codership.com>
//

//!
// @file protonet.hpp
//
// This file defines protonet interface used by gcomm.
//


#ifndef GCOMM_PROTONET_HPP
#define GCOMM_PROTONET_HPP

#include "gu_uri.hpp"
#include "gu_datetime.hpp"
#include "protostack.hpp"


#include "socket.hpp"

#include <deque>

#ifndef GCOMM_PROTONET_MAX_VERSION
#define GCOMM_PROTONET_MAX_VERSION 0
#endif // GCOMM_PROTONET_MAX_VERSION

namespace gcomm
{
    // Forward declarations
    class Protonet;
}

//!
// Abstract Protonet interface class
//
class gcomm::Protonet
{
public:
    Protonet(const std::string& type, int version)
        :
        protos_ (),
        version_(version),
        type_   (type)
    { }

    virtual ~Protonet() { }

    //!
    // Insert Protostack to be handled by Protonet
    //
    // @param pstack Pointer to Protostack
    //
    void insert(Protostack* pstack);

    //!
    // Erase Protostack from Protonet to stop dispatching events
    // to Protostack
    //
    // @param pstack Pointer to Protostack
    //
    void erase(Protostack* pstack);

    //!
    // Create new Socket
    //
    // @param uri URI to specify Socket type
    //
    // @return Socket
    //
    virtual gcomm::SocketPtr socket(const gu::URI& uri) = 0;

    //!
    // Create new Acceptor
    //
    // @param uri URI to specify Acceptor type
    //
    // @return Acceptor
    //
    virtual Acceptor* acceptor(const gu::URI& uri) = 0;

    //!
    // Dispatch events until period p has passed or event
    // loop is interrupted.
    //
    // @param p Period to run event_loop(), negative value means forever
    //
    virtual void event_loop(const gu::datetime::Period& p) = 0;

    //!
    // Iterate over Protostacks and handle timers
    //
    // @return Time of next known timer expiration
    //
    gu::datetime::Date handle_timers();

    //!
    // Interrupt event loop
    //
    virtual void interrupt() = 0;

    //!
    // Enter Protonet critical section
    //
    virtual void enter() = 0;

    //!
    // Leave Protonet critical section
    //
    virtual void leave() = 0;

    //!
    // Factory method for creating Protonets
    //
    static Protonet* create(const std::string conf, int version = 0);

    const std::string& get_type() const { return type_; }
protected:
    std::deque<Protostack*> protos_;
    int version_;
    static const int max_version_ = GCOMM_PROTONET_MAX_VERSION;
private:
    std::string type_;
};

#endif // GCOMM_PROTONET_HPP
