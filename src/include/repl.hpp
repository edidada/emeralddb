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
#include <boost/thread/thread.hpp>
#include <ctime>

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
#define REPL_OP_HEARTBEAT       9
#define REPL_OP_REQUEST_VOTE   10
#define REPL_VOTE_GRANTED      11
#define REPL_ROLE_CANDIDATE     2

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
   int _nodeID ;
   int _term ;
   int _votedTerm ;
   int _votedFor ;
   time_t _lastHeartbeat ;
   std::vector<std::pair<std::string, int> > _peers ;
   boost::thread _electionThread ;
   bool _remoteApply ;
   boost::mutex _electionMutex ;

   void electionLoop () ;
   void sendHeartbeat ( const std::string &host, int port ) ;
   bool requestVote ( const std::string &host, int port,
                      int term, int candidateID ) ;

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
      _nodeID = 0 ;
      _term = 0 ;
      _votedTerm = 0 ;
      _votedFor = 0 ;
      _lastHeartbeat = 0 ;
      _remoteApply = false ;
   }
   ~replManager () ;
   int initialize ( int role, const char *peer, const char *peers,
                    int servicePort, const char *dbPath, rtn *rtnMgr ) ;
   int start () ;
   void stop () ;
   int prepare ( int txnID, const std::vector<mvsWriteOp> &writes ) ;
   int commitTxn ( int txnID ) ;
   int abortTxn ( int txnID ) ;
   bool isPrimary () const
   {
      return REPL_ROLE_PRIMARY == _role ;
   }
   bool isWritable () const
   {
      return isPrimary() || _peers.empty() ;
   }
   bool isRemoteApply () const
   {
      return _remoteApply ;
   }
   void setRemoteApply ( bool value )
   {
      _remoteApply = value ;
   }
   int getTerm () const
   {
      return _term ;
   }
   void onHeartbeat ( int term ) ;
   bool onVoteRequest ( int term, int candidateID ) ;
} ;

#endif
