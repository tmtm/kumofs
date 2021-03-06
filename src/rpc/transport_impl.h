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
#ifndef RPC_TRANSPORT_IMPL_H__
#define RPC_TRANSPORT_IMPL_H__

namespace rpc {


inline basic_transport::basic_transport(int fd,
		basic_shared_session s, transport_manager* mgr) :
	m_fd(fd),
	m_session(s),
	m_manager(mgr) { }

inline basic_transport::~basic_transport() { }


inline transport_manager* basic_transport::get_manager()
{
	return m_manager;
}

inline basic_shared_session basic_transport::shutdown()
{
	::shutdown(m_fd, SHUT_RD);  // FIXME
	return m_session;
}

inline transport::transport(int fd, basic_shared_session& s,
		transport_manager* mgr) :
	basic_transport(fd, s, mgr),
	connection<transport>(fd)
{
	m_session->bind_transport(this);
}

inline transport::~transport()
{
	if(m_session) {
		m_session->unbind_transport(this, m_session);
	}
}


inline void basic_transport::process_request(method_id method, msgobj param,
		msgid_t msgid, auto_zone& z)
{
	if(!m_session) {
		throw std::runtime_error("session unbound");
	}
	m_session->process_request(m_session, method, param, msgid, z);
}

inline void transport::process_request(method_id method, msgobj param,
		msgid_t msgid, auto_zone& z)
{
	basic_transport::process_request(method, param, msgid, z);
}


inline void basic_transport::process_response(msgobj result, msgobj error,
		msgid_t msgid, auto_zone& z)
{
	m_session->process_response(m_session, result, error, msgid, z);
}

inline void transport::process_response(msgobj res, msgobj err,
		msgid_t msgid, auto_zone& z)
{
	basic_transport::process_response(res, err, msgid, z);
}


inline void basic_transport::send_data(
		const char* buf, size_t buflen,
		void (*finalize)(void*), void* data)
{
	wavy::request req(finalize, data);
	wavy::write(m_fd, buf, buflen, req);
}

inline void basic_transport::send_datav(
		vrefbuffer* buf,
		void (*finalize)(void*), void* data)
{
	wavy::request req(finalize, data);
	wavy::writev(m_fd, buf->vector(), buf->vector_size(), req);
}


}  // namespace rpc

#endif /* rpc/transport_impl.h */

