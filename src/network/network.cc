/*
    Mosh: the mobile shell
    Copyright 2012 Keith Winstein

    This program is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 3 of the License, or
    (at your option) any later version.

    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with this program.  If not, see <http://www.gnu.org/licenses/>.

    In addition, as a special exception, the copyright holders give
    permission to link the code of portions of this program with the
    OpenSSL library under certain conditions as described in each
    individual source file, and distribute linked combinations including
    the two.

    You must obey the GNU General Public License in all respects for all
    of the code used other than OpenSSL. If you modify file(s) with this
    exception, you may extend this exception to your version of the
    file(s), but you are not obligated to do so. If you do not wish to do
    so, delete this exception statement from your version. If you delete
    this exception statement from all source files in the program, then
    also delete it here.
*/

#include "config.h"

#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/ip.h>
#include <arpa/inet.h>
#include <assert.h>
#include <errno.h>
#include <unistd.h>
#include <netdb.h>
#include <string.h>

#include "dos_assert.h"
#include "byteorder.h"
#include "network.h"
#include "crypto.h"

#include "timestamp.h"

using namespace std;
using namespace Network;
using namespace Crypto;

const uint64_t DIRECTION_MASK = uint64_t(1) << 63;
const uint64_t SEQUENCE_MASK = uint64_t(-1) ^ DIRECTION_MASK;

/* Read in packet from coded string */
Packet::Packet( string coded_packet, Session *session )
  : seq( -1 ),
    direction( TO_SERVER ),
    timestamp( -1 ),
    timestamp_reply( -1 ),
    payload()
{
  Message message = session->decrypt( coded_packet );

  direction = (message.nonce.val() & DIRECTION_MASK) ? TO_CLIENT : TO_SERVER;
  seq = message.nonce.val() & SEQUENCE_MASK;

  dos_assert( message.text.size() >= 2 * sizeof( uint16_t ) );

  uint16_t *data = (uint16_t *)message.text.data();
  timestamp = be16toh( data[ 0 ] );
  timestamp_reply = be16toh( data[ 1 ] );

  payload = string( message.text.begin() + 2 * sizeof( uint16_t ), message.text.end() );
}

/* Output coded string from packet */
string Packet::tostring( Session *session )
{
  uint64_t direction_seq = (uint64_t( direction == TO_CLIENT ) << 63) | (seq & SEQUENCE_MASK);

  uint16_t ts_net[ 2 ] = { static_cast<uint16_t>( htobe16( timestamp ) ),
                           static_cast<uint16_t>( htobe16( timestamp_reply ) ) };

  string timestamps = string( (char *)ts_net, 2 * sizeof( uint16_t ) );

  return session->encrypt( Message( Nonce( direction_seq ), timestamps + payload ) );
}

InternetAddress::InternetAddress() {
  remote_addr_len = sizeof(remote_addr.in6);
  memset(&remote_addr.in6, '\0', remote_addr_len);
  remote_addr.in6.sin6_family = AF_INET6;
}

InternetAddress::InternetAddress( struct sockaddr_in *sa )
  : remote_addr_len( 0 )
{
  if(sa) {
    memcpy(&remote_addr, sa, sizeof(struct sockaddr_in));
    remote_addr_len = sizeof(struct sockaddr_in);
  } else {
    memset(&remote_addr, '\0', sizeof(remote_addr));
  }
}

InternetAddress::InternetAddress( struct sockaddr_in6 *sa )
  : remote_addr_len( 0 )
{
  if(sa) {
    memcpy(&remote_addr, sa, sizeof(struct sockaddr_in6));
    remote_addr_len = sizeof(struct sockaddr_in6);
  } else {
    memset(&remote_addr, '\0', sizeof(remote_addr));
  }
}

InternetAddress::InternetAddress( struct sockaddr_storage *sa, int len )
  : remote_addr_len( 0 )
{
  if(sa) {
    memcpy(&remote_addr, sa, len);
    remote_addr_len = len;
  } else {
    memset(&remote_addr, '\0', sizeof(remote_addr));
  }
}

InternetAddress::InternetAddress( const char *hostname, const char *port, int socktype )
  : remote_addr_len( 0 )
{
  setViaLookup(hostname, port, socktype);
}

void InternetAddress::setViaLookup(const char *hostname, const char *port, int socktype ) {
  struct addrinfo hints, *res;

  memset(&hints,'\0',sizeof(hints));
  hints.ai_family = AF_UNSPEC;
  hints.ai_socktype = socktype;
  hints.ai_flags = AI_PASSIVE | AI_NUMERICHOST;
  int status = getaddrinfo(hostname,port,&hints,&res);
  if(status < 0) {
    throw new NetworkException(gai_strerror(status), errno);
  }

  memcpy(&remote_addr, res->ai_addr, res->ai_addrlen);
  remote_addr_len = res->ai_addrlen;
  freeaddrinfo(res);
}

int InternetAddress::getPort() {
  if(remote_addr.ss.ss_family == AF_INET) {
    return ntohs(remote_addr.in.sin_port);
  } else {
    return ntohs(remote_addr.in6.sin6_port);
  }
}

void InternetAddress::setPort(int port) {
  if(remote_addr.ss.ss_family == AF_INET) {
    remote_addr.in.sin_port = htons(port);
  } else {
    remote_addr.in6.sin6_port = htons(port);
  }
}

std::string InternetAddress::getAddress() {
  char remote_addr_str[INET6_ADDRSTRLEN];

  inet_ntop( remote_addr.ss.ss_family,
      remote_addr.ss.ss_family == AF_INET ?
      (void *)&remote_addr.in.sin_addr :
      (void *)&remote_addr.in6.sin6_addr,
      remote_addr_str, sizeof( remote_addr_str )
      );
  return std::string( remote_addr_str );
}

void InternetAddress::setAddressBindAny() {
  remote_addr.in6.sin6_family = AF_INET6;
  memcpy(&remote_addr.in6.sin6_addr, &in6addr_any, sizeof(struct in6_addr));
  remote_addr_len = sizeof(remote_addr.in6);
}

bool InternetAddress::operator!=( const InternetAddress &b ) {
  return ! (*this == b);
}

bool InternetAddress::operator==( const InternetAddress &b ) {
  if(remote_addr.ss.ss_family != b.remote_addr.ss.ss_family) {
    return false;
  }
  if(remote_addr.ss.ss_family == AF_INET) {
    return (remote_addr.in.sin_addr.s_addr == b.remote_addr.in.sin_addr.s_addr) &&
      (remote_addr.in.sin_port == b.remote_addr.in.sin_port);
  } else {
    return IN6_ARE_ADDR_EQUAL(remote_addr.in6.sin6_addr.s6_addr32, b.remote_addr.in6.sin6_addr.s6_addr32) &&
      (remote_addr.in6.sin6_port == b.remote_addr.in6.sin6_port);
  }
}

InternetAddress & InternetAddress::operator=( const InternetAddress &rhs ) {
  if(this != &rhs) {
    memcpy(&remote_addr, &rhs.remote_addr, rhs.remote_addr_len);
    remote_addr_len = rhs.remote_addr_len;
  }
  return *this;
}

std::string InternetAddress::toString() {
  char line[200];
  snprintf(line, sizeof(line), "addr: %s\n", this->getAddress().c_str());
  std::string str = std::string( line );

  snprintf(line, sizeof(line), "family: %d\n", remote_addr.ss.ss_family);
  str.append( line );

  snprintf(line, sizeof(line), "addrlen: %d\n", remote_addr_len);
  str.append( line );

  snprintf(line, sizeof(line), "port: %d\n", this->getPort());
  str.append( line );

  return str;
}

Packet Connection::new_packet( string &s_payload )
{
  uint16_t outgoing_timestamp_reply = -1;

  uint64_t now = timestamp();

  if ( now - saved_timestamp_received_at < 1000 ) { /* we have a recent received timestamp */
    /* send "corrected" timestamp advanced by how long we held it */
    outgoing_timestamp_reply = saved_timestamp + (now - saved_timestamp_received_at);
    saved_timestamp = -1;
    saved_timestamp_received_at = 0;
  }

  Packet p( next_seq++, direction, timestamp16(), outgoing_timestamp_reply, s_payload );

  return p;
}

void Connection::setup( )
{
  /* create socket */
  sock = socket( remote_addr.getFamily(), SOCK_DGRAM, 0 );
  if ( sock < 0 ) {
    throw NetworkException( "socket", errno );
  }

  /* Disable path MTU discovery */
#ifdef HAVE_IP_MTU_DISCOVER
  char flag = IP_PMTUDISC_DONT;
  socklen_t optlen = sizeof( flag );
  if ( setsockopt( sock, IPPROTO_IP, IP_MTU_DISCOVER, &flag, optlen ) < 0 ) {
    throw NetworkException( "setsockopt", errno );
  }
#endif

  /* set diffserv values to AF42 + ECT */
  uint8_t dscp = IPTOS_ECN_ECT0 | IPTOS_DSCP_AF42;
  if(remote_addr.getFamily() == AF_INET6) {
    if ( setsockopt( sock, IPPROTO_IPV6, IPV6_TCLASS, &dscp, 1) < 0 ) {
      //    perror( "setsockopt( IP_TOS )" );
    }
  } else {
    if ( setsockopt( sock, IPPROTO_IP, IP_TOS, &dscp, 1) < 0 ) {
      //    perror( "setsockopt( IP_TOS )" );
    }
  }
}

Connection::Connection( const char *desired_ip, const char *desired_port ) /* server */
  : sock( -1 ),
    has_remote_addr( false ),
    remote_addr(),
    server( true ),
    MTU( SEND_MTU ),
    key(),
    session( key ),
    direction( TO_CLIENT ),
    next_seq( 0 ),
    saved_timestamp( -1 ),
    saved_timestamp_received_at( 0 ),
    expected_receiver_seq( 0 ),
    RTT_hit( false ),
    SRTT( 1000 ),
    RTTVAR( 500 ),
    have_send_exception( false ),
    send_exception()
{
  /* The mosh wrapper always gives an IP request, in order
     to deal with multihomed servers. The port is optional. */

  /* If an IP request is given, we try to bind to that IP, but we also
     try INADDR_ANY. If a port request is given, we bind only to that port. */

  int socket_family = 0;

  /* try to bind to desired IP first */
  if ( desired_ip ) {
    try {
      remote_addr.setViaLookup(desired_ip, desired_port, SOCK_DGRAM);
      setup();
      socket_family = remote_addr.getFamily();
      if ( try_bind( ) ) {
        return;
      }
    } catch ( const NetworkException& e ) {
    }
  }

  /* now try any local interface */
  if(desired_port) {
    remote_addr.setViaLookup(NULL, desired_port, SOCK_DGRAM);
  } else {
    remote_addr.setAddressBindAny();
  }
  if(socket_family != remote_addr.getFamily()) {
    close(sock);
    setup();
  }
  try {
    if ( try_bind( ) ) {
      return;
    }
  } catch ( const NetworkException& e ) {
    throw; /* this time it's fatal */
  }

  assert( false );
  throw NetworkException( "Could not bind", errno );
}

bool Connection::try_bind()
{
  int search_low = PORT_RANGE_LOW, search_high = PORT_RANGE_HIGH;

  if ( remote_addr.getPort() != 0 ) { /* port preference */
    search_low = search_high = remote_addr.getPort();
  }

  for ( int i = search_low; i <= search_high; i++ ) {
    remote_addr.setPort( i );

    if ( bind( sock, remote_addr.toSockaddr(), remote_addr.sockaddrLen() ) == 0 ) {
      return true;
    } else if ( i == search_high ) { /* last port to search */
      fprintf( stderr, "Failed binding to %s:%d : %s\n",
               remote_addr.getAddress().c_str(),
	       remote_addr.getPort(),
               strerror( errno ) );
      throw NetworkException( "bind", errno );
    }
  }

  assert( false );
  return false;
}

Connection::Connection( const char *key_str, const char *ip, int port ) /* client */
  : sock( -1 ),
    has_remote_addr( true ),
    remote_addr(ip, "", SOCK_DGRAM),
    server( false ),
    MTU( SEND_MTU ),
    key( key_str ),
    session( key ),
    direction( TO_SERVER ),
    next_seq( 0 ),
    saved_timestamp( -1 ),
    saved_timestamp_received_at( 0 ),
    expected_receiver_seq( 0 ),
    RTT_hit( false ),
    SRTT( 1000 ),
    RTTVAR( 500 ),
    have_send_exception( false ),
    send_exception()
{
  remote_addr.setPort(port);

  setup();
}

void Connection::send( string s )
{
  assert( has_remote_addr );

  Packet px = new_packet( s );

  string p = px.tostring( &session );

  ssize_t bytes_sent = sendto( sock, p.data(), p.size(), 0,
			       remote_addr.toSockaddr(), remote_addr.sockaddrLen() );

  if ( bytes_sent == static_cast<ssize_t>( p.size() ) ) {
    have_send_exception = false;
  } else {
    /* Notify the frontend on sendto() failure, but don't alter control flow.
       sendto() success is not very meaningful because packets can be lost in
       flight anyway. */
    have_send_exception = true;
    send_exception = NetworkException( "sendto", errno );
  }
}

string Connection::recv( void )
{
  struct sockaddr_storage packet_remote_addr;

  char buf[ Session::RECEIVE_MTU ];

  socklen_t addrlen = sizeof( packet_remote_addr );

  ssize_t received_len = recvfrom( sock, buf, Session::RECEIVE_MTU, 0, (sockaddr *)&packet_remote_addr, &addrlen );

  if ( received_len < 0 ) {
    throw NetworkException( "recvfrom", errno );
  }

  if ( received_len > Session::RECEIVE_MTU ) {
    char buffer[ 2048 ];
    snprintf( buffer, 2048, "Received oversize datagram (size %d) and limit is %d\n",
	      static_cast<int>( received_len ), Session::RECEIVE_MTU );
    throw NetworkException( buffer, errno );
  }

  Packet p( string( buf, received_len ), &session );

  dos_assert( p.direction == (server ? TO_SERVER : TO_CLIENT) ); /* prevent malicious playback to sender */

  if ( p.seq >= expected_receiver_seq ) { /* don't use out-of-order packets for timestamp or targeting */
    expected_receiver_seq = p.seq + 1; /* this is security-sensitive because a replay attack could otherwise
					  screw up the timestamp and targeting */

    if ( p.timestamp != uint16_t(-1) ) {
      saved_timestamp = p.timestamp;
      saved_timestamp_received_at = timestamp();
    }

    if ( p.timestamp_reply != uint16_t(-1) ) {
      uint16_t now = timestamp16();
      double R = timestamp_diff( now, p.timestamp_reply );

      if ( R < 5000 ) { /* ignore large values, e.g. server was Ctrl-Zed */
	if ( !RTT_hit ) { /* first measurement */
	  SRTT = R;
	  RTTVAR = R / 2;
	  RTT_hit = true;
	} else {
	  const double alpha = 1.0 / 8.0;
	  const double beta = 1.0 / 4.0;
	  
	  RTTVAR = (1 - beta) * RTTVAR + ( beta * fabs( SRTT - R ) );
	  SRTT = (1 - alpha) * SRTT + ( alpha * R );
	}
      }
    }

    /* auto-adjust to remote host */
    has_remote_addr = true;

    if ( server ) { /* only client can roam */
      InternetAddress new_remote_addr( &packet_remote_addr, addrlen );

      if(new_remote_addr != remote_addr) {
        remote_addr = new_remote_addr;
        fprintf( stderr, "Server now attached to client at %s:%d\n",
            remote_addr.getAddress().c_str(),
            remote_addr.getPort());
      }
    }
  }

  return p.payload; /* we do return out-of-order or duplicated packets to caller */
}

int Connection::port( void ) const
{
  struct sockaddr_storage local_addr_sockaddr;
  socklen_t addrlen = sizeof( local_addr_sockaddr );

  if ( getsockname( sock, (sockaddr *)&local_addr_sockaddr, &addrlen ) < 0 ) {
    throw NetworkException( "getsockname", errno );
  }

  InternetAddress local_addr(&local_addr_sockaddr, addrlen);

  return local_addr.getPort();
}

uint64_t Network::timestamp( void )
{
  return frozen_timestamp();
}

uint16_t Network::timestamp16( void )
{
  uint16_t ts = timestamp() % 65536;
  if ( ts == uint16_t(-1) ) {
    ts++;
  }
  return ts;
}

uint16_t Network::timestamp_diff( uint16_t tsnew, uint16_t tsold )
{
  int diff = tsnew - tsold;
  if ( diff < 0 ) {
    diff += 65536;
  }
  
  assert( diff >= 0 );
  assert( diff <= 65535 );

  return diff;
}

uint64_t Connection::timeout( void ) const
{
  uint64_t RTO = lrint( ceil( SRTT + 4 * RTTVAR ) );
  if ( RTO < MIN_RTO ) {
    RTO = MIN_RTO;
  } else if ( RTO > MAX_RTO ) {
    RTO = MAX_RTO;
  }
  return RTO;
}

Connection::~Connection()
{
  if ( close( sock ) < 0 ) {
    throw NetworkException( "close", errno );
  }
}
