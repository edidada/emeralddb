/*******************************************************************************
   Primary/secondary replication implementation.
*******************************************************************************/
#include "repl.hpp"
#include "pd.hpp"
#include "pmd.hpp"
#include "rtn.hpp"
#include <vector>
#include <boost/thread/thread.hpp>

static int recvExact ( ossSocket &sock, char *buffer, int length )
{
   int received = 0 ;
   while ( received < length )
   {
      int rc = sock.recv ( buffer + received, length - received, 1000000 ) ;
      if ( EDB_TIMEOUT == rc )
      {
         continue ;
      }
      if ( rc )
      {
         return rc ;
      }
      received += length - received ; // recv fills the requested buffer
   }
   return EDB_OK ;
}

void secondaryLoop ( replManager *manager, int listenPort, rtn *rtnMgr )
{
   ossSocket listener ( (unsigned int)listenPort ) ;
   if ( listener.initSocket() || listener.bind_listen() )
   {
      PD_LOG ( PDERROR, "Replication listener failed to bind port %d",
               listenPort ) ;
      return ;
   }
   while ( EDB_IS_DB_UP )
   {
      SOCKET handle = 0 ;
      int rc = listener.accept ( &handle, NULL, NULL, 1000000 ) ;
      if ( EDB_TIMEOUT == rc )
      {
         continue ;
      }
      if ( rc )
      {
         continue ;
      }
      ossSocket peer ( &handle ) ;
      while ( EDB_IS_DB_UP )
      {
         replHeader header ;
         rc = recvExact ( peer, (char*)&header, sizeof(header) ) ;
         if ( rc ||
              header.length < (int)sizeof(header) ||
              header.length > 16 * 1024 * 1024 )
         {
            break ;
         }
         int dataLen = header.length - sizeof(header) ;
         std::vector<char> payload ( dataLen > 0 ? dataLen : 1 ) ;
         if ( dataLen > 0 )
         {
            rc = recvExact ( peer, &payload[0], dataLen ) ;
            if ( rc )
            {
               break ;
            }
         }
         bson::BSONObj record ( &payload[0] ) ;
         if ( REPL_OP_INSERT == header.op )
         {
            rtnMgr->rtnInsert ( record ) ;
         }
         else if ( REPL_OP_DELETE == header.op )
         {
            rtnMgr->rtnRemove ( record ) ;
         }
      }
   }
}

replManager::~replManager ()
{
   stop () ;
   if ( _peerSock )
   {
      delete _peerSock ;
      _peerSock = NULL ;
   }
   if ( _binlog )
   {
      fclose ( _binlog ) ;
      _binlog = NULL ;
   }
}

int replManager::initialize ( int role, const char *peer, int servicePort,
                              const char *dbPath, rtn *rtnMgr )
{
   _role = role ;
   _rtn = rtnMgr ;
   _listenPort = servicePort + 1 ;
   if ( peer && *peer )
   {
      std::string text ( peer ) ;
      size_t colon = text.find ( ':' ) ;
      if ( colon != std::string::npos )
      {
         _peerHost = text.substr ( 0, colon ) ;
         _peerPort = atoi ( text.substr ( colon + 1 ).c_str() ) ;
      }
   }
   if ( isPrimary() && dbPath )
   {
      std::string logPath = std::string ( dbPath ) + ".binlog" ;
      _binlog = fopen ( logPath.c_str(), "ab" ) ;
      if ( !_binlog )
      {
         PD_LOG ( PDERROR, "Failed to open binlog %s", logPath.c_str() ) ;
         return EDB_IO ;
      }
   }
   return EDB_OK ;
}

int replManager::start ()
{
   if ( isPrimary() || _started )
   {
      return EDB_OK ;
   }
   _started = true ;
   boost::thread thread ( secondaryLoop, this, _listenPort, _rtn ) ;
   thread.detach () ;
   return EDB_OK ;
}

void replManager::stop ()
{
   // the loop checks the global database status; nothing else to join
}

int replManager::replicate ( int op, const bson::BSONObj &record )
{
   if ( !isPrimary() )
   {
      return EDB_OK ;
   }
   int dataLen   = record.objsize() ;
   int totalLen  = sizeof(replHeader) + dataLen ;
   std::vector<char> buffer ( totalLen ) ;
   replHeader *header = (replHeader*)&buffer[0] ;
   header->length = totalLen ;
   header->op     = op ;
   memcpy ( &buffer[sizeof(replHeader)], record.objdata(), dataLen ) ;
   // durable binlog first
   if ( _binlog )
   {
      fwrite ( &buffer[0], 1, totalLen, _binlog ) ;
      fflush ( _binlog ) ;
   }
   if ( _peerHost.empty() || _peerPort <= 0 )
   {
      return EDB_OK ;
   }
   if ( !_peerSock )
   {
      ossSocket *sock = new (std::nothrow) ossSocket (
            _peerHost.c_str(), (unsigned int)_peerPort ) ;
      if ( !sock || sock->initSocket() || sock->connect() )
      {
         PD_LOG ( PDWARNING, "Replication peer %s:%d unavailable",
                  _peerHost.c_str(), _peerPort ) ;
         delete sock ;
         return EDB_NETWORK ;
      }
      sock->disableNagle () ;
      _peerSock = sock ;
   }
   int rc = _peerSock->send ( &buffer[0], totalLen, 1000000 ) ;
   if ( rc )
   {
      PD_LOG ( PDWARNING, "Failed to push replication record, rc = %d", rc ) ;
      delete _peerSock ;
      _peerSock = NULL ;
      return rc ;
   }
   return EDB_OK ;
}
