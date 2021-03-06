//
// kumofs
//
// Copyright (C) 2009 FURUHASHI Sadayuki
//
//    Licensed under the Apache License, Version 2.0 (the "License");
//    you may not use this file except in compliance with the License.
//    You may obtain a copy of the License at
//
//        http://www.apache.org/licenses/LICENSE-2.0
//
//    Unless required by applicable law or agreed to in writing, software
//    distributed under the License is distributed on an "AS IS" BASIS,
//    WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
//    See the License for the specific language governing permissions and
//    limitations under the License.
//
#include "rpc/cluster.h"

namespace rpc {


cluster_transport::cluster_transport(int fd,
		basic_shared_session s, transport_manager* srv) :
	basic_transport(fd, s, srv),
	connection<cluster_transport>(fd),
	m_process_state(NULL)
{
	send_init();
	s->bind_transport(this);
}

cluster_transport::cluster_transport(int fd,
		transport_manager* srv) :
	basic_transport(fd, basic_shared_session(), srv),  // null session
	connection<cluster_transport>(fd),
	m_process_state(NULL)
{ }

cluster_transport::~cluster_transport()
{
	if(m_session) {
		m_session->unbind_transport(this, m_session);
	}
}

void cluster_transport::rebind(basic_shared_session s)
{
	if(m_session) {
		m_session->unbind_transport(this, m_session);
	}
	m_session = s;
	s->bind_transport(this);
}

void cluster_transport::send_init()
{
	msgpack::sbuffer buf;
	rpc_initmsg param(
			get_server()->m_self_addr,
			get_server()->m_self_id);
	msgpack::pack(buf, param);

	wavy::request req(&::free, buf.data());
	wavy::write(fd(), buf.data(), buf.size(), req);
	buf.release();
	LOG_TRACE("sent init message");
}

cluster* cluster_transport::get_server()
	{ return get_server(get_manager()); }

cluster* cluster_transport::get_server(transport_manager* srv)
	{ return static_cast<cluster*>(srv); }



node::node(session_manager* mgr) :
	session(mgr), m_role(-1) { }

node::~node() { }

bool node::set_role(role_type role_id)
{
	//if(m_role == -1) { m_role = role_id; return true; }
	//else { return false; }
	return __sync_bool_compare_and_swap(&m_role, -1, role_id);
}

void cluster_transport::init_message(msgobj msg, auto_zone z)
{
	rpc_message rpc(msg.convert());

	if(!rpc.is_cluster_init()) {
		// server node
		LOG_DEBUG("enter subsys state ",msg);
		if(m_session) { throw msgpack::type_error(); }

		cluster::subsys* sub =
				static_cast<cluster::subsys*>(&get_server()->subsystem());
		rebind( sub->add_session() );

		m_process_state = &cluster_transport::subsys_state;

		// re-process this message
		submit_message(msg, z);
		return;
	}

	// cluster node
	rpc_initmsg init(msg.convert());

	LOG_TRACE("receive init message: ",(uint16_t)init.role_id()," ",init.addr());

	if(!m_session) {
		if(!init.addr().connectable()) {
			throw std::runtime_error("invalid address");
		}

		send_init();
		rebind( get_server()->create_session(init.addr()) );
	}

	node* n = static_cast<node*>(m_session.get());
	if(n->set_role(init.role_id())) {
		n->m_addr = init.addr();
		// FIXME submit?
		wavy::submit(&cluster::new_node, get_server(),
				init.addr(), init.role_id(),
				mp::static_pointer_cast<node>(m_session));
	}

	m_process_state = &cluster_transport::cluster_state;
}

void cluster_transport::subsys_state(msgobj msg, msgpack::zone* newz)
{
	auto_zone z(newz);
//	LOG_TRACE("receive rpc message: ",msg);
	rpc_message rpc(msg.convert());

	if(rpc.is_request()) {
		rpc_request<msgobj> msgreq(msg.convert());
		weak_responder response(m_session, msgreq.msgid());
		get_server()->subsystem_dispatch(
				mp::static_pointer_cast<peer>(m_session),
				response, msgreq.method(), msgreq.param(), z);

	} else {
		rpc_response<msgobj, msgobj> msgres(msg.convert());
		basic_transport::process_response(
				msgres.result(), msgres.error(), msgres.msgid(), z);
	}
}

void cluster_transport::cluster_state(msgobj msg, msgpack::zone* newz)
{
	auto_zone z(newz);
//	LOG_TRACE("receive rpc message: ",msg);
	rpc_message rpc(msg.convert());

	if(rpc.is_request()) {
		rpc_request<msgobj> msgreq(msg.convert());
		weak_responder response(m_session, msgreq.msgid());
		get_server()->cluster_dispatch(
				mp::static_pointer_cast<node>(m_session),
				response, msgreq.method(), msgreq.param(), z);

	} else {
		rpc_response<msgobj, msgobj> msgres(msg.convert());
		basic_transport::process_response(
				msgres.result(), msgres.error(), msgres.msgid(), z);
	}
}



cluster::cluster(role_type self_id,
		const address& self_addr,
		unsigned int connect_timeout_msec,
		unsigned short connect_retry_limit) :
	client_base(connect_timeout_msec, connect_retry_limit),
	m_self_id(self_id),
	m_self_addr(self_addr),
	m_subsystem(this) { }

cluster::~cluster() { }

void cluster::accepted(int fd)
{
#ifndef NO_TCP_NODELAY
	int on = 1;
	::setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &on, sizeof(on));  // ignore error
#endif
#ifndef NO_SO_LINGER
	struct linger opt = {0, 0};
	::setsockopt(fd, SOL_SOCKET, SO_LINGER, (void *)&opt, sizeof(opt));  // ignore error
#endif
	wavy::add<cluster_transport>(fd, (client_base*)this);
}


shared_node cluster::get_node(const address& addr)
{
	shared_node n( get_session(addr) );
	if(!n->addr().connectable()) {
		n->m_addr = addr;
	}
	return n;
}

void cluster::transport_lost(shared_node& n)
{
	if(n->connect_retried_count() > m_connect_retry_limit) {
		LOG_DEBUG("give up to reconnect ",n->addr());
		client_base::transport_lost(n);

		if(n->is_role_set()) {
			// node is lost
			lost_node(n->addr(), n->role());
		}

	} else if(n->addr().connectable()) {
		LOG_DEBUG("reconnect to ",n->addr());
		async_connect(n->addr(), n);

	} else {
		// FIXME non-connectable node?
		LOG_DEBUG("lost node is not connectable ",n->addr());
		client_base::transport_lost(n);
	}
}



cluster::subsys::subsys(cluster* srv) :
	m_srv(srv) { }

cluster::subsys::~subsys() { }

basic_shared_session cluster::subsys::add_session()
{
	basic_shared_session s(new peer(this));
	void* k = (void*)s.get();

	pthread_scoped_lock lk(m_peers_mutex);
	m_peers.insert( peers_t::value_type(k, basic_weak_session(s)) );
	return s;
}



// connection<IMPL>::submit_message is hooked.
// transport<IMPL>::process_request won't be called.

void cluster::dispatch(
		shared_node from, weak_responder response,
		method_id method, msgobj param, auto_zone z)
{
	throw std::logic_error("cluster::dispatch called");
}

void cluster::subsys::dispatch(
		shared_peer from, weak_responder response,
		method_id method, msgobj param, auto_zone z)
{
	throw std::logic_error("cluster::subsys::dispatch called");
}


// FIXME step_timeout:
//       remove that is_role_set() == false?

}  // namespace rpc

