/*
 * The MIT License (MIT)
 *
 * Copyright (c) 2015 Microsoft Corporation
 * 
 * -=- Robust Distributed System Nucleus (rDSN) -=- 
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 */

/*
 * Description:
 *     What is this file about?
 *
 * Revision history:
 *     xxxx-xx-xx, author, first version
 *     xxxx-xx-xx, author, fix bug about xxx
 */

# ifdef _WIN32
# include <Winsock2.h>
# endif
# include <dsn/tool-api/network.h>
# include <dsn/utility/factory_store.h>
# include "message_parser_manager.h"
# include "rpc_engine.h"

# ifdef __TITLE__
# undef __TITLE__
# endif
# define __TITLE__ "network"

namespace dsn 
{
    /*static*/ join_point<void, rpc_session*> rpc_session::on_rpc_session_connected("rpc.session.connected");
    /*static*/ join_point<void, rpc_session*> rpc_session::on_rpc_session_disconnected("rpc.session.disconnected");

    rpc_session::~rpc_session()
    {
        clear_send_queue(false);

        {
            utils::auto_lock<utils::ex_lock_nr> l(_lock);
            dassert(0 == _sending_msgs.size(), "sending queue is not cleared yet");
            dassert(0 == _message_count, "sending queue is not cleared yet");
        }
    }

    bool rpc_session::try_connecting()
    {
        dassert(is_client(), "must be client session");

        utils::auto_lock<utils::ex_lock_nr> l(_lock);
        if (_connect_state == SS_DISCONNECTED)
        {
            _connect_state = SS_CONNECTING;
            return true;
        }
        else
        {
            return false;
        }
    }

    void rpc_session::set_connected()
    {
        dassert(is_client(), "must be client session");

        {
            utils::auto_lock<utils::ex_lock_nr> l(_lock);
            dassert(_connect_state == SS_CONNECTING, "session must be connecting");
            _connect_state = SS_CONNECTED;
        }

        rpc_session_ptr sp = this;
        _net.on_client_session_connected(sp);

        on_rpc_session_connected.execute(this);
    }

    bool rpc_session::set_disconnected()
    {
        {
            utils::auto_lock<utils::ex_lock_nr> l(_lock);
            if (_connect_state != SS_DISCONNECTED)
            {
                _connect_state = SS_DISCONNECTED;
            }
            else
            {
                return false;
            }
        }
        
        on_rpc_session_disconnected.execute(this);
        return true;
    }

    void rpc_session::clear_send_queue(bool resend_msgs)
    {
        //
        // - in concurrent case, resending _sending_msgs and _messages
        //   may not maintain the original sending order
        // - can optimize by batch sending instead of sending one by one
        //
        // however, our threading model cannot ensure in-order processing
        // of incoming messages neither, so this guarantee is not necesssary
        // and the upper applications should not always rely on this (but can
        // rely on this with a high probability).
        //

        std::vector<message_ex*> swapped_sending_msgs;
        {
            // protect _sending_msgs and _sending_buffers in lock
            utils::auto_lock<utils::ex_lock_nr> l(_lock);
            _sending_msgs.swap(swapped_sending_msgs);
            _sending_buffers.clear();
        }

        // resend pending messages if need
        for (auto& msg : swapped_sending_msgs)
        {
            if (resend_msgs)
            {
                _net.send_message(msg);
            }

            // if not resend, the message's callback will not be invoked until timeout,
            // it's too slow - let's try to mimic the failure by recving an empty reply
            else if (msg->header->context.u.is_request 
                && !msg->header->context.u.is_forwarded)
            {
                _net.on_recv_reply(msg->header->id, nullptr, 0);
            }

            // added in rpc_engine::reply (for server) or rpc_session::send_message (for client)
            msg->release_ref();
        }

        while (true)
        {
            dlink* msg;
            {
                utils::auto_lock<utils::ex_lock_nr> l(_lock);
                msg = _messages.next();
                if (msg == &_messages)
                    break;

                msg->remove();
                --_message_count;
            }
                        
            auto rmsg = CONTAINING_RECORD(msg, message_ex, dl);
            rmsg->io_session = nullptr;

            if (resend_msgs)
            {
                _net.send_message(rmsg);
            }

            // if not resend, the message's callback will not be invoked until timeout,
            // it's too slow - let's try to mimic the failure by recving an empty reply
            else if (rmsg->header->context.u.is_request
                && !rmsg->header->context.u.is_forwarded)
            {
                _net.on_recv_reply(rmsg->header->id, nullptr, 0);
            }

            // added in rpc_engine::reply (for server) or rpc_session::send_message (for client)
            rmsg->release_ref();
        }
    }

    inline bool rpc_session::unlink_message_for_send()
    {
        auto n = _messages.next();
        int bcount = 0;

        dbg_dassert(0 == _sending_buffers.size(), "");
        dbg_dassert(0 == _sending_msgs.size(), "");

        while (n != &_messages)
        {
            auto lmsg = CONTAINING_RECORD(n, message_ex, dl);
            auto lcount = _parser->get_buffer_count_on_send(lmsg);
            if (bcount > 0 && bcount + lcount > _max_buffer_block_count_per_send)
            {
                break;
            }

            _sending_buffers.resize(bcount + lcount);
            auto rcount = _parser->get_buffers_on_send(lmsg, &_sending_buffers[bcount]);
            dassert(lcount >= rcount, "");
            if (lcount != rcount)
                _sending_buffers.resize(bcount + rcount);
            bcount += rcount;
            _sending_msgs.push_back(lmsg);

            n = n->next();
            lmsg->dl.remove();
        }
        
        // added in send_message
        _message_count -= (int)_sending_msgs.size();
        return _sending_msgs.size() > 0;
    }
    
    DEFINE_TASK_CODE(LPC_DELAY_RPC_REQUEST_RATE, TASK_PRIORITY_COMMON, THREAD_POOL_DEFAULT)
    
    static void __delayed_rpc_session_read_next__(void* ctx)
    {
        rpc_session* s = (rpc_session*)ctx;
        s->start_read_next();
        s->release_ref(); // added in start_read_next
    }
    
    void rpc_session::start_read_next(int read_next)
    {
        // server only
        if (!is_client())
        {
            int delay_ms = _delay_server_receive_ms.exchange(0);

            // delayed read
            if (delay_ms > 0)
            {
                auto delay_task = dsn_task_create(
                    LPC_DELAY_RPC_REQUEST_RATE,
                    __delayed_rpc_session_read_next__,
                    this
                    );
                this->add_ref(); // released in __delayed_rpc_session_read_next__
                dsn_task_call(delay_task, delay_ms);
            }
            else
            {
                do_read(read_next);
            }
        }
        else
        {
            do_read(read_next);
        }
    }

    int rpc_session::prepare_parser()
    {
        if (_reader._buffer_occupied < sizeof(uint32_t))
            return sizeof(uint32_t) - _reader._buffer_occupied;

        auto hdr_format = message_parser::get_header_type(_reader._buffer.data());
        if (hdr_format == NET_HDR_INVALID)
        {
            hdr_format = _net.unknown_msg_hdr_format();

            if (hdr_format == NET_HDR_INVALID)
            {
                derror("invalid header type, remote_client = %s, header_type = '%s'",
                       _remote_addr.to_string(),
                       message_parser::get_debug_string(_reader._buffer.data()).c_str()
                    );
                return -1;
            }
        }
        _parser = _net.new_message_parser(hdr_format);
        dinfo("message parser created, remote_client = %s, header_format = %s",
              _remote_addr.to_string(), hdr_format.to_string());

        return 0;
    }
    
    void rpc_session::send_message(message_ex* msg)
    {
        msg->add_ref(); // released in on_send_completed

        msg->io_session = this;

        dassert(_parser, "parser should not be null when send");
        _parser->prepare_on_send(msg);

        uint64_t sig;
        {
            utils::auto_lock<utils::ex_lock_nr> l(_lock);
            msg->dl.insert_before(&_messages);
            ++_message_count;

            if (SS_CONNECTED == _connect_state && !_is_sending_next)
            {
                _is_sending_next = true;
                sig = _message_sent + 1;
                unlink_message_for_send();
            }
            else
            {
                return;
            }
        }

        this->send(sig);
    }

    bool rpc_session::cancel(message_ex* request)
    {
        if (request->io_session.get() != this)
            return false;

        {
            utils::auto_lock<utils::ex_lock_nr> l(_lock);
            if (request->dl.is_alone())
                return false;

            request->dl.remove();
            --_message_count;
        }

        // added in rpc_engine::reply (for server) or rpc_session::send_message (for client)
        request->release_ref();
        request->io_session = nullptr;
        return true;
    }
    
    void rpc_session::on_send_completed(uint64_t signature)
    {
        uint64_t sig = 0;
        {
            utils::auto_lock<utils::ex_lock_nr> l(_lock);
            if (signature != 0)
            {
                dassert(_is_sending_next
                    && signature == _message_sent + 1,
                    "sent msg must be sending");
                _is_sending_next = false;

                // the _sending_msgs may have been cleared when reading of the rpc_session is failed.
                if (_sending_msgs.size() == 0)
                {
                    dassert(_connect_state == SS_DISCONNECTED,
                            "assume sending queue is cleared due to session closed");
                    return;
                }
                
                for (auto& msg : _sending_msgs)
                {
                    // added in rpc_engine::reply (for server) or rpc_session::send_message (for client)
                    msg->release_ref();
                    _message_sent++;
                }
                _sending_msgs.clear();
                _sending_buffers.clear();
            }
            
            if (!_is_sending_next)
            {
                if (unlink_message_for_send())
                {
                    sig = _message_sent + 1;
                    _is_sending_next = true;
                }
            }
        }

        // for next send messages
        if (sig != 0)
            this->send(sig);
    }

    bool rpc_session::has_pending_out_msgs()
    {
        utils::auto_lock<utils::ex_lock_nr> l(_lock);
        return !_messages.is_alone();
    }

    rpc_session::rpc_session(
        connection_oriented_network& net, 
        ::dsn::rpc_address remote_addr,
        message_parser_ptr& parser,
        bool is_client
        )
        : _net(net),
        _remote_addr(remote_addr),
        _max_buffer_block_count_per_send(net.max_buffer_block_count_per_send()),
        _reader(net.message_buffer_block_size()),
        _parser(parser),
        _is_client(is_client),
        _matcher(_net.engine()->matcher()),
        _is_sending_next(false),
        _message_count(0),
        _connect_state(is_client ? SS_DISCONNECTED : SS_CONNECTED),
        _message_sent(0),
        _delay_server_receive_ms(0)
    {
        if (!is_client)
        {
            on_rpc_session_connected.execute(this);
        }
    }

    bool rpc_session::on_disconnected(bool is_write)
    {
        bool ret;
        if (set_disconnected())
        {
            rpc_session_ptr sp = this;
            if (is_client())
            {
                _net.on_client_session_disconnected(sp);
            }
            else
            {
                _net.on_server_session_disconnected(sp);
            }

            ret = true;
        }
        else
        {
            ret = false;
        }

        if (is_write)
        {
            clear_send_queue(false);
        }

        return ret;
    }

    bool rpc_session::on_recv_message(message_ex* msg, int delay_ms)
    {
        if (msg->header->from_address.is_invalid())
            msg->header->from_address = _remote_addr;
        msg->to_address = _net.address();
        msg->io_session = this;

        if (msg->header->context.u.is_request)
        {
            // ATTENTION: need to check if self connection occurred.
            //
            // When we try to connect some socket in the same host, if we don't bind the client to a specific port,
            // operating system will provide ephemeral port for us. If it's happened to be the one we want to connect to,
            // it causes self connection.
            //
            // The case is:
            // - this session is a client session
            // - the remote address is in the same host
            // - the remote address is not listened, which means the remote port is not occupied
            // - operating system chooses the remote port as client's ephemeral port
            if (is_client() && msg->header->from_address == _net.engine()->primary_address())
            {
                derror("self connection detected, address = %s", msg->header->from_address.to_string());
                dassert(msg->get_count() == 0,
                    "message should not be referenced by anybody so far");
                delete msg;
                return false;
            }

            dbg_dassert(!is_client(), "only rpc server session can recv rpc requests");
            _net.on_recv_request(msg, delay_ms);
        }

        // both rpc server session and rpc client session can receive rpc reply
        // specially, rpc client session can receive general rpc reply,  
        // and rpc server session can receive forwarded rpc reply  
        else
        {
            _matcher->on_recv_reply(&_net, msg->header->id, msg, delay_ms);
        }

        return true;
    }
    
    ////////////////////////////////////////////////////////////////////////////////////////////////
    network::network(rpc_engine* srv, network* inner_provider)
        : _engine(srv), _client_hdr_format(NET_HDR_DSN), _unknown_msg_header_format(NET_HDR_INVALID)
    {   
        _message_buffer_block_size = 1024 * 64;
        _max_buffer_block_count_per_send = 64; // TODO: windows, how about the other platforms?
        _send_queue_threshold = (int)dsn_config_get_value_uint64(
            "network", "send_queue_threshold",
            4 * 1024, "send queue size above which throttling is applied"
            );

        _unknown_msg_header_format = network_header_format::from_string(
            dsn_config_get_value_string(
                "network", 
                "unknown_message_header_format", 
                NET_HDR_INVALID.to_string(),
                "format for unknown message headers, default is NET_HDR_INVALID"
                ), NET_HDR_INVALID);
    }

    void network::reset_parser_attr(network_header_format client_hdr_format, int message_buffer_block_size)
    {
        _client_hdr_format = client_hdr_format;
        _message_buffer_block_size = message_buffer_block_size;
    }

    service_node* network::node() const
    {
        return _engine->node();
    }

    void network::on_recv_request(message_ex* msg, int delay_ms)
    {
        return _engine->on_recv_request(this, msg, delay_ms);
    }

    void network::on_recv_reply(uint64_t id, message_ex* msg, int delay_ms)
    {
        _engine->matcher()->on_recv_reply(this, id, msg, delay_ms);
    }

    message_parser* network::new_message_parser(network_header_format hdr_format)
    {
        message_parser* parser = message_parser_manager::instance().create_parser(hdr_format);
        dassert(parser, "message parser '%s' not registerd or invalid!", hdr_format.to_string());
        return parser;
    }

    std::pair<message_parser::factory2, size_t>  network::get_message_parser_info(network_header_format hdr_format)
    {
        auto& pinfo = message_parser_manager::instance().get(hdr_format);
        dassert(pinfo.factory2, "message parser '%s' not registerd or invalid!", hdr_format.to_string());
        return std::make_pair(pinfo.factory2, pinfo.parser_size);
    }

    uint32_t network::get_local_ipv4()
    {
        static const char* explicit_host = dsn_config_get_value_string(
            "network", "explicit_host_address",
            "", "explicit host name or ip (v4) assigned to this node (e.g., service ip for pods in kubernets)"
            );

        static const char* inteface = dsn_config_get_value_string(
            "network", "primary_interface",
            "", "network interface name used to init primary ipv4 address, if empty, means using the first \"eth\" prefixed non-loopback ipv4 address");

        uint32_t ip = 0;

        if (strlen(explicit_host) > 0)
        {
            ip = dsn_ipv4_from_host(explicit_host);
        }

        if (0 == ip)
        {
            ip = dsn_ipv4_local(inteface);
        }
        
        if (0 == ip)
        {
            char name[128];
            if (gethostname(name, sizeof(name)) != 0)
            {
                dassert(false, "gethostname failed, err = %s", strerror(errno));
            }
            ip = dsn_ipv4_from_host(name);
        }

        return ip;
    }

    connection_oriented_network::connection_oriented_network(rpc_engine* srv, network* inner_provider)
        : network(srv, inner_provider)
    {        
    }

    void connection_oriented_network::inject_drop_message(message_ex* msg, bool is_send)
    {
        rpc_session_ptr s = msg->io_session;
        if (s == nullptr)
        {
            // - if io_session == nulltr, there must be is_send == true;
            // - but if is_send == true, there may be is_session != nullptr, when it is a
            //   normal (not forwarding) reply message from server to client, in which case
            //   the io_session has also been set.
            dassert(is_send, "received message should always has io_session set");
            utils::auto_read_lock l(_clients_lock);
            auto it = _clients.find(msg->to_address);
            if (it != _clients.end())
            {
                s = it->second;
            }
        }

        if (s != nullptr)
        {
            s->close_on_fault_injection();
        }
    }

    void connection_oriented_network::send_message(message_ex* request)
    {
        rpc_session_ptr client = nullptr;
        auto& to = request->to_address;

        // TODO: thread-local client ptr cache
        {
            utils::auto_read_lock l(_clients_lock);
            auto it = _clients.find(to);
            if (it != _clients.end())
            {
                client = it->second;
            }
        }

        int scount = 0;
        bool new_client = false;
        if (nullptr == client.get())
        {
            utils::auto_write_lock l(_clients_lock);
            auto it = _clients.find(to);
            if (it != _clients.end())
            {
                client = it->second;
            }
            else
            {
                client = create_client_session(to);
                _clients.insert(client_sessions::value_type(to, client));
                new_client = true;
            }
            scount = (int)_clients.size();
        }

        // init connection if necessary
        if (new_client) 
        {
            ddebug("client session created, remote_server = %s, current_count = %d",
                   client->remote_address().to_string(), scount);
            client->connect();
        }

        // rpc call
        client->send_message(request);
    }

    rpc_session_ptr connection_oriented_network::get_server_session(::dsn::rpc_address ep)
    {
        utils::auto_read_lock l(_servers_lock);
        auto it = _servers.find(ep);
        return it != _servers.end() ? it->second : nullptr;
    }

    void connection_oriented_network::on_server_session_accepted(rpc_session_ptr& s)
    {
        int scount = 0;
        {
            utils::auto_write_lock l(_servers_lock);
            auto pr = _servers.insert(server_sessions::value_type(s->remote_address(), s));
            if (pr.second)
            {
                // nothing to do 
            }
            else
            {
                pr.first->second = s;
                dwarn("server session already exists, remote_client = %s, preempted",
                      s->remote_address().to_string());
            }
            scount = (int)_servers.size();
        }

        ddebug("server session accepted, remote_client = %s, current_count = %d",
               s->remote_address().to_string(), scount);
    }

    void connection_oriented_network::on_server_session_disconnected(rpc_session_ptr& s)
    {
        int scount = 0;
        bool r = false;
        {
            utils::auto_write_lock l(_servers_lock);
            auto it = _servers.find(s->remote_address());
            if (it != _servers.end() && it->second.get() == s.get())
            {
                _servers.erase(it);
                r = true;
            }      
            scount = (int)_servers.size();
        }

        if (r)
        {
            ddebug("server session disconnected, remote_client = %s, current_count = %d",
                   s->remote_address().to_string(), scount);
        }
    }

    rpc_session_ptr connection_oriented_network::get_client_session(::dsn::rpc_address ep)
    {
        utils::auto_read_lock l(_clients_lock);
        auto it = _clients.find(ep);
        return it != _clients.end() ? it->second : nullptr;
    }

    void connection_oriented_network::on_client_session_connected(rpc_session_ptr& s)
    {
        int scount = 0;
        bool r = false;
        {
            utils::auto_read_lock l(_clients_lock);
            auto it = _clients.find(s->remote_address());
            if (it != _clients.end() && it->second.get() == s.get())
            {
                r = true;
            }
            scount = (int)_clients.size();
        }

        if (r)
        {
            ddebug("client session connected, remote_server = %s, current_count = %d",
                   s->remote_address().to_string(), scount);
        }
    }

    void connection_oriented_network::on_client_session_disconnected(rpc_session_ptr& s)
    {
        int scount = 0;
        bool r = false;
        {
            utils::auto_write_lock l(_clients_lock);
            auto it = _clients.find(s->remote_address());
            if (it != _clients.end() && it->second.get() == s.get())
            {
                _clients.erase(it);
                r = true;
            }
            scount = (int)_clients.size();
        }

        if (r)
        {
            ddebug("client session disconnected, remote_server = %s, current_count = %d",
                   s->remote_address().to_string(), scount);
        }
    }
}
