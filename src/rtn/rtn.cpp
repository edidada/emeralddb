/*******************************************************************************
   Copyright (C) 2013 SequoiaDB Software Inc.

   This program is free software: you can redistribute it and/or modify
   it under the terms of the GNU Affero General Public License, version 3,
   as published by the Free Software Foundation.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
   GNU Affero General Public License for more details.

   You should have received a copy of the GNU Affero General Public License
   along with this program. If not, see <http://www.gnu.org/license/>.
*******************************************************************************/
#include "core.hpp"
#include "rtn.hpp"
#include "pd.hpp"
#include "pmd.hpp"
#include "qry.hpp"
#include "mvs.hpp"

using namespace bson ;

rtn::rtn() :
_dmsFile(NULL),
_ixmBucketMgr(NULL)
{
}

rtn::~rtn()
{
   if ( _ixmBucketMgr )
   {
      delete _ixmBucketMgr ;
   }
   if ( _dmsFile )
   {
      delete _dmsFile ;
   }
}

int rtn::rtnInitialize ( const char *dbPath )
{
   int rc = EDB_OK ;
   std::string indexPath ;
   if ( dbPath )
   {
      indexPath = std::string ( dbPath ) + ".idx" ;
   }
   _ixmBucketMgr = new ( std::nothrow ) ixmBucketManager () ;
   if ( !_ixmBucketMgr )
   {
      rc = EDB_OOM ;
      PD_LOG ( PDERROR, "Failed to new bucket manager" ) ;
      goto error ;
   }
   _dmsFile = new(std::nothrow) dmsFile ( _ixmBucketMgr ) ;
   if ( !_dmsFile )
   {
      rc = EDB_OOM ;
      PD_LOG ( PDERROR, "Failed to new dms file" ) ;
      goto error ;
   }
   rc = _ixmBucketMgr->initialize ( indexPath.empty() ? NULL : indexPath.c_str() ) ;
   if ( rc )
   {
      PD_LOG ( PDERROR, "Failed to call bucketMgr initialize, rc = %d", rc ) ;
      goto error ;
   }
   // init dms
   rc = _dmsFile->initialize ( pmdGetKRCB()->getDataFilePath () ) ;
   if ( rc )
   {
      PD_LOG ( PDERROR, "Failed to call dms initialize, rc = %d", rc ) ;
      goto error ;
   }
done :
   return rc ;
error :
   goto done ;
}

int rtn::insertRaw ( BSONObj &record )
{
   int rc = EDB_OK ;
   dmsRecordID recordID ;
   BSONObj outRecord ;
   rc = _ixmBucketMgr->isIDExist ( record ) ;
   PD_RC_CHECK ( rc, PDERROR, "Failed to call isIDExist, rc = %d", rc ) ;
   rc = _dmsFile->insert ( record, outRecord, recordID ) ;
   PD_RC_CHECK ( rc, PDERROR, "Failed to call dms insert, rc = %d", rc ) ;
   rc = _ixmBucketMgr->createIndex ( outRecord, recordID ) ;
   PD_RC_CHECK ( rc, PDERROR, "Failed to call ixmCreateIndex, rc = %d", rc ) ;
done :
   return rc ;
error :
   goto done ;
}

int rtn::removeRaw ( BSONObj &record )
{
   int rc = EDB_OK ;
   dmsRecordID recordID ;
   rc = _ixmBucketMgr->removeIndex ( record, recordID ) ;
   PD_RC_CHECK ( rc, PDERROR, "Failed to call ixm removeIndex, rc = %d", rc ) ;
   rc = _dmsFile->remove ( recordID ) ;
   PD_RC_CHECK ( rc, PDERROR, "Failed to call dms remove, rc = %d", rc ) ;
done :
   return rc ;
error :
   goto done ;
}

int rtn::findRaw ( BSONObj &inRecord, BSONObj &outRecord )
{
   int rc = EDB_OK ;
   dmsRecordID recordID ;
   if ( !qry::hasIdLookup ( inRecord ) )
   {
      int matches = 0 ;
      rc = _dmsFile->scan ( inRecord, outRecord, matches ) ;
      PD_RC_CHECK ( rc, PDERROR, "Failed to scan records, rc = %d", rc ) ;
      goto done ;
   }
   rc = _ixmBucketMgr->findIndex ( inRecord, recordID ) ;
   PD_RC_CHECK ( rc, PDERROR, "Failed to call ixm findIndex, rc = %d", rc ) ;
   rc = _dmsFile->find ( recordID, outRecord ) ;
   PD_RC_CHECK ( rc, PDERROR, "Failed to call dms find, rc = %d", rc ) ;
done :
   return rc ;
error :
   goto done ;
}

int rtn::rtnInsert ( BSONObj &record )
{
   EDB_KRCB *krcb = pmdGetKRCB () ;
   txnManager *txn = krcb->getTxnMgr () ;
   pmdEDUCB *cb = pmdGetCurrentEDU () ;
   if ( txn->isApplying() )
   {
      return insertRaw ( record ) ;
   }
   if ( cb && cb->isInTxn() )
   {
      return txn->stageWrite ( cb->getTxnID(), TXN_OP_INSERT, record ) ;
   }
   mvsWriteOp write ;
   write.key  = mvsKeyOf ( record ) ;
   write.op   = 1 ;
   write.data.assign ( record.objdata(), record.objsize() ) ;
   std::vector<mvsWriteOp> writes ( 1, write ) ;
   mvsManager *mvs = krcb->getMvsMgr () ;
   int rc = mvs->commit ( writes, mvs->nextTS() ) ;
   if ( rc )
   {
      return rc ;
   }
   return insertRaw ( record ) ;
}

int rtn::rtnRemove ( BSONObj &record )
{
   EDB_KRCB *krcb = pmdGetKRCB () ;
   txnManager *txn = krcb->getTxnMgr () ;
   pmdEDUCB *cb = pmdGetCurrentEDU () ;
   if ( txn->isApplying() )
   {
      return removeRaw ( record ) ;
   }
   if ( cb && cb->isInTxn() )
   {
      return txn->stageWrite ( cb->getTxnID(), TXN_OP_DELETE, record ) ;
   }
   mvsWriteOp write ;
   write.key  = mvsKeyOf ( record ) ;
   write.op   = 2 ;
   write.data.assign ( record.objdata(), record.objsize() ) ;
   std::vector<mvsWriteOp> writes ( 1, write ) ;
   mvsManager *mvs = krcb->getMvsMgr () ;
   int rc = mvs->commit ( writes, mvs->nextTS() ) ;
   if ( rc )
   {
      return rc ;
   }
   return removeRaw ( record ) ;
}

int rtn::rtnFind ( BSONObj &inRecord, BSONObj &outRecord )
{
   EDB_KRCB *krcb = pmdGetKRCB () ;
   mvsManager *mvs = krcb->getMvsMgr () ;
   pmdEDUCB *cb = pmdGetCurrentEDU () ;
   uint64_t snapshot = mvs->currentTS () ;
   if ( cb && cb->isInTxn() )
   {
      snapshot = krcb->getTxnMgr()->getSnapshot ( cb->getTxnID() ) ;
   }
   std::string key = mvsKeyOf ( inRecord ) ;
   std::string data ;
   if ( !key.empty() && EDB_OK == mvs->visible ( key, snapshot, data ) )
   {
      outRecord = BSONObj ( data.c_str() ).copy () ;
      return EDB_OK ;
   }
   return findRaw ( inRecord, outRecord ) ;
}

int rtn::rebuildVersions ( mvsManager *mvs )
{
   std::vector<std::string> records ;
   _dmsFile->getAll ( records ) ;
   std::vector<mvsWriteOp> writes ;
   for ( size_t i = 0 ; i < records.size() ; ++i )
   {
      BSONObj record ( records[i].c_str() ) ;
      mvsWriteOp write ;
      write.key = mvsKeyOf ( record ) ;
      write.op  = 1 ;
      write.data = records[i] ;
      writes.push_back ( write ) ;
   }
   if ( writes.empty() )
   {
      return EDB_OK ;
   }
   return mvs->commit ( writes, mvs->nextTS() ) ;
}

int rtn::explain ( const BSONObj &query, BSONObj &result )
{
   BSONObjBuilder builder ;
   builder.append ( "plan", qry::planName ( query ) ) ;
   builder.append ( "estimatedCost",
                    qry::estimateCost ( query, _dmsFile->getRecordCount() ) ) ;
   builder.append ( "recordCount", (int)_dmsFile->getRecordCount() ) ;
   result = builder.obj() ;
   return EDB_OK ;
}

