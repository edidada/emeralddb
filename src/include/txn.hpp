/*******************************************************************************
   Minimal write-ahead-log based transaction manager.

   Each connection can open one transaction. Operations inside a transaction
   are written to a WAL before they are applied to the storage engine. On
   commit the WAL is flushed; on rollback the operations are undone in reverse
   order. On startup, committed transactions are redone and transactions that
   never committed are undone.
*******************************************************************************/
#ifndef TXN_HPP__
#define TXN_HPP__

#include <map>
#include <string>
#include <vector>
#include <cstdio>
#include "core.hpp"
#include "bson.h"

#define TXN_WAL_MAGIC "EDBWAL01"
#define TXN_WAL_VERSION 1

#define TXN_OP_BEGIN   1
#define TXN_OP_INSERT  2
#define TXN_OP_DELETE  3
#define TXN_OP_COMMIT  4
#define TXN_OP_ROLLBACK 5

struct txnWalHeader
{
   int length ;
   int op ;
   int txnID ;
} ;

class rtn ;

class txnManager
{
private :
   FILE *_wal ;
   int _nextTxnID ;
   std::map<int, std::vector<std::pair<int, std::string> > > _undo ;
   bool _recovering ;
   std::string _walPath ;
public :
   txnManager ()
   {
      _wal = NULL ;
      _nextTxnID = 1 ;
      _recovering = false ;
   }
   ~txnManager ()
   {
      if ( _wal )
      {
         fclose ( _wal ) ;
      }
   }
   int initialize ( const char *walPath ) ;
   int recover ( rtn *rtnMgr ) ;
   int begin ( int &txnID ) ;
   int logOperation ( int txnID, int op, const bson::BSONObj &record ) ;
   int commit ( int txnID ) ;
   int rollback ( int txnID, rtn *rtnMgr ) ;
   bool isRecovering () const
   {
      return _recovering ;
   }
} ;

#endif
