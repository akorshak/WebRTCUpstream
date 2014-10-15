/*
 * libjingle
 * Copyright 2011, Google Inc.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *
 *  1. Redistributions of source code must retain the above copyright notice,
 *     this list of conditions and the following disclaimer.
 *  2. Redistributions in binary form must reproduce the above copyright notice,
 *     this list of conditions and the following disclaimer in the documentation
 *     and/or other materials provided with the distribution.
 *  3. The name of the author may not be used to endorse or promote products
 *     derived from this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE AUTHOR ``AS IS'' AND ANY EXPRESS OR IMPLIED
 * WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO
 * EVENT SHALL THE AUTHOR BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 * SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO,
 * PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS;
 * OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY,
 * WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR
 * OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF
 * ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#include "webrtc/p2p/base/basicpacketsocketfactory.h"

#include "webrtc/p2p/base/asyncstuntcpsocket.h"
#include "webrtc/p2p/base/stun.h"
#include "webrtc/base/asynctcpsocket.h"
#include "webrtc/base/asyncudpsocket.h"
#include "webrtc/base/logging.h"
#include "webrtc/base/nethelpers.h"
#include "webrtc/base/physicalsocketserver.h"
#include "webrtc/base/scoped_ptr.h"
#include "webrtc/base/socketadapters.h"
#include "webrtc/base/ssladapter.h"
#include "webrtc/base/thread.h"

namespace rtc {

BasicPacketSocketFactory::BasicPacketSocketFactory()
    : thread_(Thread::Current()),
      socket_factory_(NULL) {
}

BasicPacketSocketFactory::BasicPacketSocketFactory(Thread* thread)
    : thread_(thread),
      socket_factory_(NULL) {
}

BasicPacketSocketFactory::BasicPacketSocketFactory(
    SocketFactory* socket_factory)
    : thread_(NULL),
      socket_factory_(socket_factory) {
}

BasicPacketSocketFactory::~BasicPacketSocketFactory() {
}

AsyncPacketSocket* BasicPacketSocketFactory::CreateUdpSocket(
    const SocketAddress& address, int min_port, int max_port) {
  // UDP sockets are simple.
  rtc::AsyncSocket* socket =
      socket_factory()->CreateAsyncSocket(
          address.family(), SOCK_DGRAM);
  if (!socket) {
    return NULL;
  }
  if (BindSocket(socket, address, min_port, max_port) < 0) {
    LOG(LS_ERROR) << "UDP bind failed with error "
                    << socket->GetError();
    delete socket;
    return NULL;
  }
  return new rtc::AsyncUDPSocket(socket);
}

AsyncPacketSocket* BasicPacketSocketFactory::CreateServerTcpSocket(
    const SocketAddress& local_address, int min_port, int max_port, int opts) {

  // Fail if TLS is required.
  if (opts & PacketSocketFactory::OPT_TLS) {
    LOG(LS_ERROR) << "TLS support currently is not available.";
    return NULL;
  }

  rtc::AsyncSocket* socket =
      socket_factory()->CreateAsyncSocket(local_address.family(),
                                          SOCK_STREAM);
  if (!socket) {
    return NULL;
  }

  if (BindSocket(socket, local_address, min_port, max_port) < 0) {
    LOG(LS_ERROR) << "TCP bind failed with error "
                  << socket->GetError();
    delete socket;
    return NULL;
  }

  // If using SSLTCP, wrap the TCP socket in a pseudo-SSL socket.
  if (opts & PacketSocketFactory::OPT_SSLTCP) {
    ASSERT(!(opts & PacketSocketFactory::OPT_TLS));
    socket = new rtc::AsyncSSLSocket(socket);
  }

  // Set TCP_NODELAY (via OPT_NODELAY) for improved performance.
  // See http://go/gtalktcpnodelayexperiment
  socket->SetOption(rtc::Socket::OPT_NODELAY, 1);

  if (opts & PacketSocketFactory::OPT_STUN)
    return new cricket::AsyncStunTCPSocket(socket, true);

  return new rtc::AsyncTCPSocket(socket, true);
}

AsyncPacketSocket* BasicPacketSocketFactory::CreateClientTcpSocket(
    const SocketAddress& local_address, const SocketAddress& remote_address,
    const ProxyInfo& proxy_info, const std::string& user_agent, int opts) {

  rtc::AsyncSocket* socket =
      socket_factory()->CreateAsyncSocket(local_address.family(), SOCK_STREAM);
  if (!socket) {
    return NULL;
  }

  if (BindSocket(socket, local_address, 0, 0) < 0) {
    LOG(LS_ERROR) << "TCP bind failed with error "
                  << socket->GetError();
    delete socket;
    return NULL;
  }

  // If using a proxy, wrap the socket in a proxy socket.
  if (proxy_info.type == rtc::PROXY_SOCKS5) {
    socket = new rtc::AsyncSocksProxySocket(
        socket, proxy_info.address, proxy_info.username, proxy_info.password);
  } else if (proxy_info.type == rtc::PROXY_HTTPS) {
    socket = new rtc::AsyncHttpsProxySocket(
        socket, user_agent, proxy_info.address,
        proxy_info.username, proxy_info.password);
  }

  // If using TLS, wrap the socket in an SSL adapter.
  if (opts & PacketSocketFactory::OPT_TLS) {
    ASSERT(!(opts & PacketSocketFactory::OPT_SSLTCP));

    rtc::SSLAdapter* ssl_adapter = rtc::SSLAdapter::Create(socket);
    if (!ssl_adapter) {
      return NULL;
    }

    socket = ssl_adapter;

    if (ssl_adapter->StartSSL(remote_address.hostname().c_str(), false) != 0) {
      delete ssl_adapter;
      return NULL;
    }

  // If using SSLTCP, wrap the TCP socket in a pseudo-SSL socket.
  } else if (opts & PacketSocketFactory::OPT_SSLTCP) {
    ASSERT(!(opts & PacketSocketFactory::OPT_TLS));
    socket = new rtc::AsyncSSLSocket(socket);
  }

  if (socket->Connect(remote_address) < 0) {
    LOG(LS_ERROR) << "TCP connect failed with error "
                  << socket->GetError();
    delete socket;
    return NULL;
  }

  // Finally, wrap that socket in a TCP or STUN TCP packet socket.
  AsyncPacketSocket* tcp_socket;
  if (opts & PacketSocketFactory::OPT_STUN) {
    tcp_socket = new cricket::AsyncStunTCPSocket(socket, false);
  } else {
    tcp_socket = new rtc::AsyncTCPSocket(socket, false);
  }

  // Set TCP_NODELAY (via OPT_NODELAY) for improved performance.
  // See http://go/gtalktcpnodelayexperiment
  tcp_socket->SetOption(rtc::Socket::OPT_NODELAY, 1);

  return tcp_socket;
}

AsyncResolverInterface* BasicPacketSocketFactory::CreateAsyncResolver() {
  return new rtc::AsyncResolver();
}

int BasicPacketSocketFactory::BindSocket(
    AsyncSocket* socket, const SocketAddress& local_address,
    int min_port, int max_port) {
  int ret = -1;
  if (min_port == 0 && max_port == 0) {
    // If there's no port range, let the OS pick a port for us.
    ret = socket->Bind(local_address);
  } else {
    // Otherwise, try to find a port in the provided range.
    for (int port = min_port; ret < 0 && port <= max_port; ++port) {
      ret = socket->Bind(rtc::SocketAddress(local_address.ipaddr(),
                                                  port));
    }
  }
  return ret;
}

SocketFactory* BasicPacketSocketFactory::socket_factory() {
  if (thread_) {
    ASSERT(thread_ == Thread::Current());
    return thread_->socketserver();
  } else {
    return socket_factory_;
  }
}

}  // namespace rtc
