/*******************************************************************************
   Asynchronous primary/secondary replication.

   The primary appends every write to a binlog and pushes it to the secondary
   over a dedicated TCP port (service port + 1). The secondary listens on that
   port and applies the received operations through the record layer.
*******************************************************************************/
#ifndef REPL_HPP__
#define REPL_HPP__

#include <string>
#include <cstdio>
#include "core.hpp"
#include "bson.h"
#include "ossSocket.hpp"

#define REPL_ROLE_PRIMARY   0
#define REPL_ROLE_SECONDARY 1

#define REPL_OP_INSERT 1
#define REPL_OP_DELETE 2

struct replHeader
{
   int length ;
   int op ;
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
   }
   ~replManager () ;
   int initialize ( int role, const char *peer, int servicePort,
                    const char *dbPath, rtn *rtnMgr ) ;
   int start () ;
   void stop () ;
   int replicate ( int op, const bson::BSONObj &record ) ;
   bool isPrimary () const
   {
      return REPL_ROLE_PRIMARY == _role ;
   }
} ;

#endif
