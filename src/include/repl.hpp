/*******************************************************************************
   Asynchronous primary/secondary replication.

   The primary appends every write to a binlog and pushes it to the secondary
   over a dedicated TCP port (service port + 1). The secondary listens on that
   port and applies the received operations through the record layer.
*******************************************************************************/
#ifndef REPL_HPP__
#define REPL_HPP__

#include <string>
#include <map>
#include <cstdio>
#include "core.hpp"
#include "bson.h"
#include "ossSocket.hpp"
#include "mvs.hpp"
#include <boost/thread/mutex.hpp>

#define REPL_ROLE_PRIMARY   0
#define REPL_ROLE_SECONDARY 1

#define REPL_OP_INSERT  1
#define REPL_OP_DELETE  2
#define REPL_OP_PREPARE 3
#define REPL_OP_COMMIT  4
#define REPL_OP_ABORT   5
#define REPL_VOTE_YES   6
#define REPL_VOTE_NO    7
#define REPL_ACK        8

struct replHeader
{
   int length ;
   int op ;
   int txnID ;
   int count ;
} ;

class rtn ;

class replManager
{
private :
   int _role ;
   std::string _peerHost ;
   int _peerPort ;
   int _listenPort ;
   FILE *_binlog ;
   ossSocket *_peerSock ;
   rtn *_rtn ;
   bool _started ;
   std::map<int, std::vector<mvsWriteOp> > _prepared ;
   std::string _twoPcPath ;
   FILE *_twoPcLog ;
   boost::mutex _peerMutex ;

public :
   void log2PC ( const char *action, int txnID ) ;
   void stagePrepared ( int txnID, const std::vector<mvsWriteOp> &ops ) ;
   void applyPrepared ( int txnID ) ;
   void discardPrepared ( int txnID ) ;
   int ensurePeer () ;
   int sendMessage ( int op, int txnID, int count,
                     const std::vector<char> &payload ) ;
public :
   replManager ()
   {
      _role = REPL_ROLE_PRIMARY ;
      _peerPort = 0 ;
      _listenPort = 0 ;
      _binlog = NULL ;
      _peerSock = NULL ;
      _rtn = NULL ;
      _started = false ;
      _twoPcLog = NULL ;
   }
   ~replManager () ;
   int initialize ( int role, const char *peer, int servicePort,
                    const char *dbPath, rtn *rtnMgr ) ;
   int start () ;
   void stop () ;
   int prepare ( int txnID, const std::vector<mvsWriteOp> &writes ) ;
   int commitTxn ( int txnID ) ;
   int abortTxn ( int txnID ) ;
   bool isPrimary () const
   {
      return REPL_ROLE_PRIMARY == _role ;
   }
} ;

#endif
