/*******************************************************************************
   Two-phase-commit replication implementation.
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
      received = length ;
   }
   return EDB_OK ;
}

static void writeInt ( std::vector<char> &buffer, int &offset, int value )
{
   memcpy ( &buffer[offset], &value, sizeof(value) ) ;
   offset += sizeof(value) ;
}

static int readInt ( const char *buffer, int &offset )
{
   int value = 0 ;
   memcpy ( &value, buffer + offset, sizeof(value) ) ;
   offset += sizeof(value) ;
   return value ;
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
      if ( EDB_TIMEOUT == rc || rc )
      {
         continue ;
      }
      ossSocket peer ( &handle ) ;
      while ( EDB_IS_DB_UP )
      {
         replHeader header ;
         rc = recvExact ( peer, (char*)&header, sizeof(header) ) ;
         if ( rc || header.length < (int)sizeof(header) ||
              header.length > 64 * 1024 * 1024 )
         {
            break ;
         }
         int payloadLen = header.length - sizeof(header) ;
         std::vector<char> payload ( payloadLen > 0 ? payloadLen : 1 ) ;
         if ( payloadLen > 0 )
         {
            rc = recvExact ( peer, &payload[0], payloadLen ) ;
            if ( rc )
            {
               break ;
            }
         }
         int op = REPL_ACK ;
         if ( REPL_OP_PREPARE == header.op )
         {
            std::vector<mvsWriteOp> ops ;
            int offset = 0 ;
            for ( int i = 0 ; i < header.count ; ++i )
            {
               if ( offset + 8 > payloadLen )
               {
                  break ;
               }
               int len = readInt ( &payload[0], offset ) ;
               int writeOp = readInt ( &payload[0], offset ) ;
               if ( len <= 0 || offset + len > payloadLen )
               {
                  break ;
               }
               mvsWriteOp write ;
               write.op = writeOp ;
               write.data.assign ( &payload[offset], len ) ;
               bson::BSONObj record ( write.data.c_str() ) ;
               write.key = mvsKeyOf ( record ) ;
               ops.push_back ( write ) ;
               offset += len ;
            }
            manager->stagePrepared ( header.txnID, ops ) ;
            op = REPL_VOTE_YES ;
         }
         else if ( REPL_OP_COMMIT == header.op )
         {
            manager->applyPrepared ( header.txnID ) ;
            op = REPL_ACK ;
         }
         else if ( REPL_OP_ABORT == header.op )
         {
            manager->discardPrepared ( header.txnID ) ;
            op = REPL_ACK ;
         }
         else
         {
            bson::BSONObj record ( &payload[0] ) ;
            if ( REPL_OP_INSERT == header.op )
            {
               rtnMgr->rtnInsert ( record ) ;
            }
            else if ( REPL_OP_DELETE == header.op )
            {
               rtnMgr->rtnRemove ( record ) ;
            }
            continue ;
         }
         replHeader reply ;
         reply.length = sizeof(reply) ;
         reply.op     = op ;
         reply.txnID  = header.txnID ;
         reply.count  = 0 ;
         peer.send ( (const char*)&reply, sizeof(reply) ) ;
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
   if ( _twoPcLog )
   {
      fclose ( _twoPcLog ) ;
      _twoPcLog = NULL ;
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
   if ( dbPath )
   {
      std::string logPath = std::string ( dbPath ) + ".binlog" ;
      if ( isPrimary() )
      {
         _binlog = fopen ( logPath.c_str(), "ab" ) ;
         if ( !_binlog )
         {
            PD_LOG ( PDERROR, "Failed to open binlog %s", logPath.c_str() ) ;
            return EDB_IO ;
         }
      }
      else
      {
         _twoPcPath = std::string ( dbPath ) + ".2pc" ;
         _twoPcLog = fopen ( _twoPcPath.c_str(), "ab" ) ;
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
}

void replManager::log2PC ( const char *action, int txnID )
{
   if ( _twoPcLog )
   {
      fprintf ( _twoPcLog, "%s %d\n", action, txnID ) ;
      fflush ( _twoPcLog ) ;
   }
}

void replManager::stagePrepared ( int txnID,
                                  const std::vector<mvsWriteOp> &ops )
{
   _prepared[txnID] = ops ;
   log2PC ( "PREPARE", txnID ) ;
}

void replManager::applyPrepared ( int txnID )
{
   std::vector<mvsWriteOp> ops = _prepared[txnID] ;
   log2PC ( "COMMIT", txnID ) ;
   _prepared.erase ( txnID ) ;
   for ( size_t i = 0 ; i < ops.size() ; ++i )
   {
      bson::BSONObj record ( ops[i].data.c_str() ) ;
      if ( 1 == ops[i].op )
      {
         _rtn->rtnInsert ( record ) ;
      }
      else
      {
         _rtn->rtnRemove ( record ) ;
      }
   }
}

void replManager::discardPrepared ( int txnID )
{
   log2PC ( "ABORT", txnID ) ;
   _prepared.erase ( txnID ) ;
}

int replManager::ensurePeer ()
{
   if ( _peerSock )
   {
      return EDB_OK ;
   }
   if ( _peerHost.empty() || _peerPort <= 0 )
   {
      return EDB_OK ;
   }
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
   return EDB_OK ;
}

int replManager::sendMessage ( int op, int txnID, int count,
                               const std::vector<char> &payload )
{
   boost::mutex::scoped_lock lock ( _peerMutex ) ;
   if ( !isPrimary() )
   {
      return EDB_OK ;
   }
   if ( _peerHost.empty() || _peerPort <= 0 )
   {
      return EDB_OK ;
   }
   int rc = ensurePeer () ;
   if ( rc )
   {
      return rc ;
   }
   int total = sizeof(replHeader) + (int)payload.size() ;
   std::vector<char> buffer ( total ) ;
   replHeader *header = (replHeader*)&buffer[0] ;
   header->length = total ;
   header->op     = op ;
   header->txnID  = txnID ;
   header->count  = count ;
   if ( !payload.empty() )
   {
      memcpy ( &buffer[sizeof(replHeader)], &payload[0], payload.size() ) ;
   }
   rc = _peerSock->send ( &buffer[0], total, 1000000 ) ;
   if ( rc )
   {
      delete _peerSock ;
      _peerSock = NULL ;
      return rc ;
   }
   replHeader reply ;
   rc = recvExact ( *_peerSock, (char*)&reply, sizeof(reply) ) ;
   if ( rc )
   {
      delete _peerSock ;
      _peerSock = NULL ;
      return rc ;
   }
   if ( REPL_OP_PREPARE == op )
   {
      return REPL_VOTE_YES == reply.op ? EDB_OK : EDB_CONFLICT ;
   }
   return REPL_ACK == reply.op ? EDB_OK : EDB_NETWORK ;
}

int replManager::prepare ( int txnID, const std::vector<mvsWriteOp> &writes )
{
   if ( !isPrimary() || _peerHost.empty() )
   {
      return EDB_OK ;
   }
   int payloadLen = 0 ;
   for ( size_t i = 0 ; i < writes.size() ; ++i )
   {
      payloadLen += 8 + (int)writes[i].data.size() ;
   }
   std::vector<char> payload ( payloadLen > 0 ? payloadLen : 1 ) ;
   int offset = 0 ;
   for ( size_t i = 0 ; i < writes.size() ; ++i )
   {
      int len = (int)writes[i].data.size() ;
      writeInt ( payload, offset, len ) ;
      writeInt ( payload, offset, writes[i].op ) ;
      memcpy ( &payload[offset], writes[i].data.data(), len ) ;
      offset += len ;
   }
   return sendMessage ( REPL_OP_PREPARE, txnID,
                        (int)writes.size(), payload ) ;
}

int replManager::commitTxn ( int txnID )
{
   std::vector<char> empty ;
   return sendMessage ( REPL_OP_COMMIT, txnID, 0, empty ) ;
}

int replManager::abortTxn ( int txnID )
{
   std::vector<char> empty ;
   return sendMessage ( REPL_OP_ABORT, txnID, 0, empty ) ;
}
