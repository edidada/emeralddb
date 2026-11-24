/*******************************************************************************
   Transaction manager implementation (see txn.hpp).
*******************************************************************************/
#include "txn.hpp"
#include "pd.hpp"
#include "rtn.hpp"
#include "pmd.hpp"

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

int txnManager::begin ( int &txnID, uint64_t snapshot )
{
   txnID = _nextTxnID++ ;
   _undo[txnID] = std::vector<std::pair<int, std::string> > () ;
   _writes[txnID] = std::vector<mvsWriteOp> () ;
   _snapshots[txnID] = snapshot ;
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
   return EDB_OK ;
}

int txnManager::stageWrite ( int txnID, int op, const bson::BSONObj &record )
{
   int rc = logOperation ( txnID, op, record ) ;
   if ( rc )
   {
      return rc ;
   }
   mvsWriteOp write ;
   write.key  = mvsKeyOf ( record ) ;
   write.op   = ( TXN_OP_DELETE == op ) ? 2 : 1 ;
   write.data.assign ( record.objdata(), record.objsize() ) ;
   _writes[txnID].push_back ( write ) ;
   return EDB_OK ;
}

uint64_t txnManager::getSnapshot ( int txnID )
{
   std::map<int, uint64_t>::iterator it = _snapshots.find ( txnID ) ;
   return it == _snapshots.end() ? 0 : it->second ;
}

int txnManager::commit ( int txnID, mvsManager *mvs, rtn *rtnMgr )
{
   std::vector<mvsWriteOp> &writes = _writes[txnID] ;
   int rc = mvs->checkConflicts ( writes, _snapshots[txnID] ) ;
   if ( rc )
   {
      rollback ( txnID, rtnMgr ) ;
      return rc ;
   }
   // Phase 1: ask the replica to prepare. If it votes no, abort locally.
   replManager *repl = pmdGetKRCB()->getReplMgr() ;
   rc = repl->prepare ( txnID, writes ) ;
   if ( rc )
   {
      rollback ( txnID, rtnMgr ) ;
      return rc ;
   }
   uint64_t ts = mvs->nextTS () ;
   rc = mvs->commit ( writes, ts ) ;
   if ( rc )
   {
      return rc ;
   }
   txnWalHeader header ;
   header.length = sizeof(header) ;
   header.op     = TXN_OP_COMMIT ;
   header.txnID  = txnID ;
   if ( _wal )
   {
      fwrite ( &header, sizeof(header), 1, _wal ) ;
      fflush ( _wal ) ;
   }
   // version store is durable; apply the committed images to the storage engine
   _applying = true ;
   for ( size_t i = 0 ; i < writes.size() ; ++i )
   {
      bson::BSONObj record ( writes[i].data.c_str() ) ;
      if ( 1 == writes[i].op )
      {
         rtnMgr->rtnInsert ( record ) ;
      }
      else
      {
         rtnMgr->rtnRemove ( record ) ;
      }
   }
   _applying = false ;
   // Phase 2: tell the replica to commit the prepared transaction.
   repl->commitTxn ( txnID ) ;
   _undo.erase ( txnID ) ;
   _writes.erase ( txnID ) ;
   _snapshots.erase ( txnID ) ;
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
   // writes are staged only, so discarding them is sufficient
   pmdGetKRCB()->getReplMgr()->abortTxn ( txnID ) ;
   _undo.erase ( txnID ) ;
   _writes.erase ( txnID ) ;
   _snapshots.erase ( txnID ) ;
   return EDB_OK ;
}

int txnManager::autoCommit ( int op, const bson::BSONObj &record,
                             mvsManager *mvs, rtn *rtnMgr )
{
   int txnID = 0 ;
   int rc = begin ( txnID, mvs->currentTS() ) ;
   if ( rc )
   {
      return rc ;
   }
   rc = stageWrite ( txnID, op, record ) ;
   if ( rc )
   {
      rollback ( txnID, rtnMgr ) ;
      return rc ;
   }
   return commit ( txnID, mvs, rtnMgr ) ;
}

int txnManager::recover ( rtn *rtnMgr, mvsManager *mvs )
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
   // Only committed transactions are replayed. Uncommitted writes were staged
   // and never applied to the version store or the storage engine, so there
   // is nothing to undo.
   std::map<int, std::vector<mvsWriteOp> > recovered ;
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
      mvsWriteOp write ;
      write.key  = mvsKeyOf ( record ) ;
      write.op   = ( TXN_OP_DELETE == records[i].op ) ? 2 : 1 ;
      write.data = records[i].image ;
      recovered[records[i].txnID].push_back ( write ) ;
   }
   for ( std::map<int, std::vector<mvsWriteOp> >::iterator it =
            recovered.begin() ; it != recovered.end() ; ++it )
   {
      uint64_t ts = mvs->nextTS () ;
      mvs->commit ( it->second, ts ) ;
      _applying = true ;
      for ( size_t i = 0 ; i < it->second.size() ; ++i )
      {
         bson::BSONObj record ( it->second[i].data.c_str() ) ;
         if ( 1 == it->second[i].op )
         {
            rtnMgr->rtnInsert ( record ) ;
         }
         else
         {
            rtnMgr->rtnRemove ( record ) ;
         }
      }
      _applying = false ;
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
