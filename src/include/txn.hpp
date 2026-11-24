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
#include "mvs.hpp"

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
   std::map<int, std::vector<mvsWriteOp> > _writes ;
   std::map<int, uint64_t> _snapshots ;
   bool _recovering ;
   bool _applying ;
   std::string _walPath ;
public :
   txnManager ()
   {
      _wal = NULL ;
      _nextTxnID = 1 ;
      _recovering = false ;
      _applying = false ;
   }
   ~txnManager ()
   {
      if ( _wal )
      {
         fclose ( _wal ) ;
      }
   }
   int initialize ( const char *walPath ) ;
   int recover ( rtn *rtnMgr, mvsManager *mvs ) ;
   bool isApplying () const
   {
      return _applying || _recovering ;
   }
   int begin ( int &txnID, uint64_t snapshot ) ;
   int logOperation ( int txnID, int op, const bson::BSONObj &record ) ;
   int stageWrite ( int txnID, int op, const bson::BSONObj &record ) ;
   uint64_t getSnapshot ( int txnID ) ;
   int commit ( int txnID, mvsManager *mvs, rtn *rtnMgr ) ;
   int rollback ( int txnID, rtn *rtnMgr ) ;
   bool isRecovering () const
   {
      return _recovering ;
   }
} ;

#endif
