/*******************************************************************************
   Transaction manager implementation (see txn.hpp).
*******************************************************************************/
#include "txn.hpp"
#include "pd.hpp"
#include "rtn.hpp"

int txnManager::initialize ( const char *walPath )
{
   if ( !walPath )
   {
      return EDB_INVALIDARG ;
   }
   _walPath = walPath ;
   _wal = fopen ( walPath, "a+b" ) ;
   if ( !_wal )
   {
      PD_LOG ( PDERROR, "Failed to open WAL %s", walPath ) ;
      return EDB_IO ;
   }
   fseek ( _wal, 0, SEEK_END ) ;
   if ( ftell ( _wal ) == 0 )
   {
      unsigned int version = TXN_WAL_VERSION ;
      fwrite ( TXN_WAL_MAGIC, 1, 8, _wal ) ;
      fwrite ( &version, sizeof(version), 1, _wal ) ;
      fflush ( _wal ) ;
   }
   return EDB_OK ;
}

int txnManager::begin ( int &txnID )
{
   txnID = _nextTxnID++ ;
   _undo[txnID] = std::vector<std::pair<int, std::string> > () ;
   txnWalHeader header ;
   header.length  = sizeof(header) ;
   header.op      = TXN_OP_BEGIN ;
   header.txnID   = txnID ;
   if ( !_wal ||
        fwrite ( &header, sizeof(header), 1, _wal ) != 1 ||
        fflush ( _wal ) )
   {
      return EDB_IO ;
   }
   return EDB_OK ;
}

int txnManager::logOperation ( int txnID, int op, const bson::BSONObj &record )
{
   int dataLen    = record.objsize() ;
   int totalLen   = sizeof(txnWalHeader) + dataLen ;
   txnWalHeader header ;
   header.length = totalLen ;
   header.op     = op ;
   header.txnID  = txnID ;
   if ( !_wal ||
        fwrite ( &header, sizeof(header), 1, _wal ) != 1 ||
        fwrite ( record.objdata(), 1, dataLen, _wal ) !=
           (size_t)dataLen ||
        fflush ( _wal ) )
   {
      return EDB_IO ;
   }
   std::string image ( record.objdata(), dataLen ) ;
   _undo[txnID].push_back ( std::make_pair ( op, image ) ) ;
   return EDB_OK ;
}

int txnManager::commit ( int txnID )
{
   txnWalHeader header ;
   header.length = sizeof(header) ;
   header.op     = TXN_OP_COMMIT ;
   header.txnID  = txnID ;
   if ( !_wal ||
        fwrite ( &header, sizeof(header), 1, _wal ) != 1 ||
        fflush ( _wal ) )
   {
      return EDB_IO ;
   }
   _undo.erase ( txnID ) ;
   return EDB_OK ;
}

int txnManager::rollback ( int txnID, rtn *rtnMgr )
{
   txnWalHeader header ;
   header.length = sizeof(header) ;
   header.op     = TXN_OP_ROLLBACK ;
   header.txnID  = txnID ;
   if ( _wal )
   {
      fwrite ( &header, sizeof(header), 1, _wal ) ;
      fflush ( _wal ) ;
   }
   std::vector<std::pair<int, std::string> > &operations = _undo[txnID] ;
   for ( int i = (int)operations.size() - 1 ; i >= 0 ; --i )
   {
      bson::BSONObj record ( operations[i].second.c_str() ) ;
      int rc = EDB_OK ;
      if ( TXN_OP_INSERT == operations[i].first )
      {
         rc = rtnMgr->rtnRemove ( record ) ;
      }
      else if ( TXN_OP_DELETE == operations[i].first )
      {
         rc = rtnMgr->rtnInsert ( record ) ;
      }
      if ( rc && rc != EDB_IXM_ID_NOT_EXIST && rc != EDB_IXM_ID_EXIST )
      {
         PD_LOG ( PDERROR, "Failed to undo operation, rc = %d", rc ) ;
      }
   }
   _undo.erase ( txnID ) ;
   return EDB_OK ;
}

int txnManager::recover ( rtn *rtnMgr )
{
   if ( !_wal )
   {
      return EDB_IO ;
   }
   rewind ( _wal ) ;
   char magic[9] = {0} ;
   unsigned int version = 0 ;
   if ( fread ( magic, 1, 8, _wal ) != 8 ||
        strcmp ( magic, TXN_WAL_MAGIC ) ||
        fread ( &version, sizeof(version), 1, _wal ) != 1 ||
        version != TXN_WAL_VERSION )
   {
      PD_LOG ( PDERROR, "Invalid WAL header, recovery skipped" ) ;
      return EDB_INVALIDARG ;
   }
   struct WalRecord
   {
      int op ;
      int txnID ;
      std::string image ;
   } ;
   std::vector<WalRecord> records ;
   std::vector<int> committed ;
   txnWalHeader header ;
   while ( fread ( &header, sizeof(header), 1, _wal ) == 1 )
   {
      if ( header.length < (int)sizeof(header) || header.length > 16 * 1024 * 1024 )
      {
         PD_LOG ( PDERROR, "Corrupted WAL record detected, recovery stopped" ) ;
         return EDB_INVALIDARG ;
      }
      WalRecord rec ;
      rec.op    = header.op ;
      rec.txnID = header.txnID ;
      int dataLen = header.length - sizeof(header) ;
      if ( dataLen > 0 )
      {
         std::vector<char> buffer ( dataLen ) ;
         if ( fread ( &buffer[0], 1, dataLen, _wal ) != (size_t)dataLen )
         {
            return EDB_INVALIDARG ;
         }
         rec.image.assign ( &buffer[0], dataLen ) ;
      }
      if ( TXN_OP_COMMIT == rec.op )
      {
         committed.push_back ( rec.txnID ) ;
      }
      records.push_back ( rec ) ;
   }
   _recovering = true ;
   // redo committed transactions in log order
   for ( size_t i = 0 ; i < records.size() ; ++i )
   {
      bool isCommitted = false ;
      for ( size_t j = 0 ; j < committed.size() ; ++j )
      {
         if ( committed[j] == records[i].txnID )
         {
            isCommitted = true ;
            break ;
         }
      }
      if ( !isCommitted ||
           ( TXN_OP_INSERT != records[i].op && TXN_OP_DELETE != records[i].op ) )
      {
         continue ;
      }
      bson::BSONObj record ( records[i].image.c_str() ) ;
      bson::BSONObj out ;
      int rc = EDB_OK ;
      if ( TXN_OP_INSERT == records[i].op )
      {
         if ( EDB_IXM_ID_NOT_EXIST == rtnMgr->rtnFind ( record, out ) )
         {
            rc = rtnMgr->rtnInsert ( record ) ;
         }
      }
      else
      {
         bson::BSONObj out ;
         if ( EDB_OK == rtnMgr->rtnFind ( record, out ) )
         {
            rc = rtnMgr->rtnRemove ( record ) ;
         }
      }
      if ( rc )
      {
         PD_LOG ( PDERROR, "Failed to redo WAL operation, rc = %d", rc ) ;
      }
   }
   // undo transactions that never committed, in reverse log order
   for ( int i = (int)records.size() - 1 ; i >= 0 ; --i )
   {
      bool isCommitted = false ;
      for ( size_t j = 0 ; j < committed.size() ; ++j )
      {
         if ( committed[j] == records[i].txnID )
         {
            isCommitted = true ;
            break ;
         }
      }
      if ( isCommitted ||
           ( TXN_OP_INSERT != records[i].op && TXN_OP_DELETE != records[i].op ) )
      {
         continue ;
      }
      bson::BSONObj record ( records[i].image.c_str() ) ;
      bson::BSONObj out ;
      int rc = EDB_OK ;
      if ( TXN_OP_INSERT == records[i].op )
      {
         if ( EDB_OK == rtnMgr->rtnFind ( record, out ) )
         {
            rc = rtnMgr->rtnRemove ( record ) ;
         }
      }
      else
      {
         if ( EDB_IXM_ID_NOT_EXIST == rtnMgr->rtnFind ( record, out ) )
         {
            rc = rtnMgr->rtnInsert ( record ) ;
         }
      }
      if ( rc )
      {
         PD_LOG ( PDERROR, "Failed to undo WAL operation, rc = %d", rc ) ;
      }
   }
   _recovering = false ;
   // restart the log from a clean header
   fclose ( _wal ) ;
   _wal = fopen ( _walPath.c_str(), "wb" ) ;
   if ( _wal )
   {
      unsigned int version = TXN_WAL_VERSION ;
      fwrite ( TXN_WAL_MAGIC, 1, 8, _wal ) ;
      fwrite ( &version, sizeof(version), 1, _wal ) ;
      fflush ( _wal ) ;
      fclose ( _wal ) ;
   }
   _wal = fopen ( _walPath.c_str(), "a+b" ) ;
   return _wal ? EDB_OK : EDB_IO ;
}
