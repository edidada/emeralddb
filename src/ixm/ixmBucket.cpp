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
#include "pd.hpp"
#include "ossHash.hpp"
#include "ixmBucket.hpp"
#include "ixmPersist.hpp"

int ixmBucketManager::isIDExist ( BSONObj &record )
{
   int rc               = EDB_OK ;
   unsigned int hashNum = 0 ;
   unsigned int random  = 0 ;
   ixmEleHash eleHash ;
   dmsRecordID recordID ;
   rc = _processData ( record, recordID, hashNum, eleHash, random ) ;
   if ( rc )
   {
      PD_LOG ( PDERROR, "Failed to process data, rc = %d", rc ) ;
      goto error ;
   }
   rc = _bucket[random]->isIDExist ( hashNum, eleHash ) ;
   if ( rc )
   {
      PD_LOG ( PDERROR, "Failed to create index, rc = %d", rc ) ;
      goto error ;
   }
done :
   return rc ;
error :
   goto done ;
}

int ixmBucketManager::createIndex ( BSONObj &record, dmsRecordID &recordID )
{
   int rc                = EDB_OK ;
   unsigned int hashNum  = 0 ;
   unsigned int random   = 0 ;
   ixmEleHash eleHash ;
   rc = _processData ( record, recordID, hashNum, eleHash, random ) ;
   PD_RC_CHECK ( rc, PDERROR, "Failed to process data, rc = %d", rc ) ;
   rc = _bucket[random]->createIndex ( hashNum, eleHash ) ;
   PD_RC_CHECK ( rc, PDERROR, "Failed to create index, rc = %d", rc ) ;
   recordID = eleHash.recordID ;
   // keep the index durable; a failed append is reported to the caller
   BSONElement element ( eleHash.data ) ;
   rc = appendEntry ( IXM_IDX_OP_INSERT, element, random, hashNum, recordID ) ;
   PD_RC_CHECK ( rc, PDERROR, "Failed to persist index entry, rc = %d", rc ) ;
done :
   return rc ;
error :
   goto done ;
}


int ixmBucketManager::findIndex ( BSONObj &record, dmsRecordID &recordID )
{
   int rc                = EDB_OK ;
   unsigned int hashNum  = 0 ;
   unsigned int random   = 0 ;
   ixmEleHash eleHash ;
   rc = _processData ( record, recordID, hashNum, eleHash, random ) ;
   PD_RC_CHECK ( rc, PDERROR, "Failed to process data, rc = %d", rc ) ;
   rc = _bucket[random]->findIndex ( hashNum, eleHash ) ;
   PD_RC_CHECK ( rc, PDERROR, "Failed to find index, rc = %d", rc ) ;
   recordID = eleHash.recordID ;
done :
   return rc ;
error :
   goto done ;
}

int ixmBucketManager::removeIndex ( BSONObj &record, dmsRecordID &recordID )
{
   int rc                = EDB_OK ;
   unsigned int hashNum  = 0 ;
   unsigned int random   = 0 ;
   ixmEleHash eleHash ;
   rc = _processData ( record, recordID, hashNum, eleHash, random ) ;
   PD_RC_CHECK ( rc, PDERROR, "Failed to process data, rc = %d", rc ) ;
   rc = _bucket[random]->removeIndex ( hashNum, eleHash ) ;
   PD_RC_CHECK ( rc, PDERROR, "Failed to remove index, rc = %d", rc ) ;
   recordID._pageID = eleHash.recordID._pageID ;
   recordID._slotID = eleHash.recordID._slotID ;
   BSONElement element ( eleHash.data ) ;
   rc = appendEntry ( IXM_IDX_OP_DELETE, element, random, hashNum, recordID ) ;
   PD_RC_CHECK ( rc, PDERROR, "Failed to persist index removal, rc = %d", rc ) ;
done :
   return rc ;
error :
   goto done ;
}

int ixmBucketManager::_processData ( BSONObj &record,
                                     dmsRecordID &recordID,
                                     unsigned int &hashNum,
                                     ixmEleHash &eleHash,
                                     unsigned int &random )
{
   int rc               = EDB_OK ;
   BSONElement element  = record.getField ( IXM_KEY_FIELDNAME ) ;
   // check if _id exists and correct
   if ( element.eoo() ||
        ( element.type() != NumberInt && element.type() != String ) )
   {
      rc = EDB_INVALIDARG ;
      PD_LOG ( PDERROR, "record must be with _id" ) ;
      goto error ;
   }
   // hash _id
   hashNum = ossHash ( element.value(), element.valuesize() ) ;
   random = hashNum % IXM_HASH_MAP_SIZE ;
   eleHash.data = element.rawdata () ;
   eleHash.recordID = recordID ;
done :
   return rc ;
error :
   goto done ;
}

int ixmBucketManager::ixmBucket::isIDExist ( unsigned int hashNum,
                                             ixmEleHash &eleHash )
{
   int rc = EDB_OK ;
   BSONElement destEle ;
   BSONElement sourEle ;
   ixmEleHash existEle ;
   std::pair<std::multimap<unsigned int, ixmEleHash>::iterator,
             std::multimap<unsigned int, ixmEleHash>::iterator> ret ;
   _mutex.get_shared () ;
   ret = _bucketMap.equal_range ( hashNum ) ;
   sourEle = BSONElement ( eleHash.data ) ;
   for ( std::multimap<unsigned int, ixmEleHash>::iterator it = ret.first ;
         it != ret.second; ++it )
   {
      existEle = it->second ;
      destEle = BSONElement ( existEle.data ) ;
      if ( sourEle.type() == destEle.type() )
      {
         if ( sourEle.valuesize() == destEle.valuesize() )
         {
            if ( !memcmp ( sourEle.value(), destEle.value(),
                           destEle.valuesize() ) )
            {
               rc = EDB_IXM_ID_EXIST ;
               PD_LOG ( PDERROR, "record _id does exist" ) ;
               goto error ;
            }
         }
      }
   }
done :
   _mutex.release_shared () ;
   return rc ;
error :
   goto done ;
}

int ixmBucketManager::ixmBucket::createIndex ( unsigned int hashNum,
                                               ixmEleHash &eleHash )
{
   int rc = EDB_OK ;
   _mutex.get() ;
   _bucketMap.insert (
      pair<unsigned int, ixmEleHash> ( hashNum, eleHash ) ) ;
   _mutex.release () ;
   return rc ;
}

int ixmBucketManager::ixmBucket::findIndex ( unsigned int hashNum,
                                             ixmEleHash &eleHash )
{
   int rc = EDB_OK ;
   BSONElement destEle ;
   BSONElement sourEle ;
   ixmEleHash existEle ;
   std::pair<std::multimap<unsigned int, ixmEleHash>::iterator,
             std::multimap<unsigned int, ixmEleHash>::iterator> ret ;
   _mutex.get_shared () ;
   ret = _bucketMap.equal_range ( hashNum ) ;
   sourEle = BSONElement ( eleHash.data ) ;
   for ( std::multimap<unsigned int, ixmEleHash>::iterator it = ret.first ;
         it != ret.second; ++it )
   {
      existEle = it->second ;
      destEle = BSONElement ( existEle.data ) ;
      if ( sourEle.type() == destEle.type() )
      {
         if ( sourEle.valuesize() == destEle.valuesize() )
         {
            if ( !memcmp ( sourEle.value(), destEle.value(),
                           destEle.valuesize() ) )
            {
               eleHash.recordID = existEle.recordID ;
               goto done ;
            }
         }
      }
   }
   rc = EDB_IXM_ID_NOT_EXIST ;
   PD_LOG ( PDERROR, "record _id does not exist, hashNum = %d", hashNum ) ;
   goto error ;
done :
   _mutex.release_shared () ;
   return rc ;
error :
   goto done ;
}

int ixmBucketManager::ixmBucket::removeIndex ( unsigned int hashNum,
                                             ixmEleHash &eleHash )
{
   int rc = EDB_OK ;
   BSONElement destEle ;
   BSONElement sourEle ;
   ixmEleHash existEle ;
   std::pair<std::multimap<unsigned int, ixmEleHash>::iterator,
             std::multimap<unsigned int, ixmEleHash>::iterator> ret ;
   _mutex.get () ;
   ret = _bucketMap.equal_range ( hashNum ) ;
   sourEle = BSONElement ( eleHash.data ) ;
   for ( std::multimap<unsigned int, ixmEleHash>::iterator it = ret.first ;
         it != ret.second; ++it )
   {
      existEle = it->second ;
      destEle = BSONElement ( existEle.data ) ;
      if ( sourEle.type() == destEle.type() )
      {
         if ( sourEle.valuesize() == destEle.valuesize() )
         {
            if ( !memcmp ( sourEle.value(), destEle.value(),
                           destEle.valuesize() ) )
            {
               eleHash.recordID = existEle.recordID ;
               _bucketMap.erase ( it ) ;
               goto done ;
            }
         }
      }
   }
   rc = EDB_INVALIDARG ;
   PD_LOG ( PDERROR, "record _id does not exist" ) ;
   goto error ;
done :
   _mutex.release () ;
   return rc ;
error :
   goto done ;
}

int ixmBucketManager::appendEntry ( unsigned char op,
                                     const BSONElement &element,
                                     unsigned int bucket,
                                     unsigned int hashNum,
                                     const dmsRecordID &rid )
{
   if ( !_persistFile )
   {
      return EDB_OK ;
   }
   unsigned int keyLen  = (unsigned int)element.size() ;
   const char *key      = element.rawdata() ;
   unsigned char type   = (unsigned char)element.type() ;
   if ( fwrite ( &bucket, sizeof(bucket), 1, _persistFile ) != 1 ||
        fwrite ( &hashNum, sizeof(hashNum), 1, _persistFile ) != 1 ||
        fwrite ( &rid._pageID, sizeof(rid._pageID), 1, _persistFile ) != 1 ||
        fwrite ( &rid._slotID, sizeof(rid._slotID), 1, _persistFile ) != 1 ||
        fwrite ( &keyLen, sizeof(keyLen), 1, _persistFile ) != 1 ||
        fwrite ( &op, sizeof(op), 1, _persistFile ) != 1 ||
        fwrite ( &type, sizeof(type), 1, _persistFile ) != 1 ||
        fwrite ( key, 1, keyLen, _persistFile ) != keyLen )
   {
      return EDB_IO ;
   }
   fflush ( _persistFile ) ;
   return EDB_OK ;
}

int ixmBucketManager::loadFromDisk ( const char *path )
{
   FILE *file = fopen ( path, "rb" ) ;
   if ( !file )
   {
      return EDB_IO ;
   }
   char magic[9] = {0} ;
   unsigned int version = 0 ;
   if ( fread ( magic, 1, 8, file ) != 8 ||
        strcmp ( magic, IXM_INDEX_MAGIC ) ||
        fread ( &version, sizeof(version), 1, file ) != 1 ||
        version != IXM_INDEX_VERSION )
   {
      fclose ( file ) ;
      return EDB_INVALIDARG ;
   }
   while ( true )
   {
      unsigned int bucket = 0, hash = 0, pageID = 0, slotID = 0, keyLen = 0 ;
      unsigned char op = 0, type = 0 ;
      if ( fread ( &bucket, sizeof(bucket), 1, file ) != 1 )
      {
         break ; // normal end of the append-only log
      }
      if ( fread ( &hash, sizeof(hash), 1, file ) != 1 ||
           fread ( &pageID, sizeof(pageID), 1, file ) != 1 ||
           fread ( &slotID, sizeof(slotID), 1, file ) != 1 ||
           fread ( &keyLen, sizeof(keyLen), 1, file ) != 1 ||
           fread ( &op, sizeof(op), 1, file ) != 1 ||
           fread ( &type, sizeof(type), 1, file ) != 1 ||
           keyLen == 0 || keyLen > 4096 )
      {
         fclose ( file ) ;
         return EDB_INVALIDARG ;
      }
      char *key = (char*)malloc ( keyLen + 1 ) ;
      if ( !key )
      {
         fclose ( file ) ;
         return EDB_OOM ;
      }
      if ( fread ( key, 1, keyLen, file ) != keyLen )
      {
         free ( key ) ;
         fclose ( file ) ;
         return EDB_INVALIDARG ;
      }
      key[keyLen] = 0 ;
      ixmEleHash eleHash ;
      eleHash.data = key ;
      eleHash.recordID._pageID = pageID ;
      eleHash.recordID._slotID = slotID ;
      int rc = EDB_OK ;
      if ( IXM_IDX_OP_INSERT == op )
      {
         rc = _bucket[bucket % IXM_HASH_MAP_SIZE]->createIndex ( hash, eleHash ) ;
         if ( EDB_OK == rc )
         {
            _ownedKeys.push_back ( key ) ;
         }
      }
      else if ( IXM_IDX_OP_DELETE == op )
      {
         rc = _bucket[bucket % IXM_HASH_MAP_SIZE]->removeIndex ( hash, eleHash ) ;
      }
      if ( rc )
      {
         free ( key ) ;
         fclose ( file ) ;
         return rc ;
      }
      if ( IXM_IDX_OP_DELETE == op )
      {
         free ( key ) ;
      }
   }
   fclose ( file ) ;
   _persistFile = fopen ( path, "ab" ) ;
   if ( !_persistFile )
   {
      return EDB_IO ;
   }
   _persisted = true ;
   return EDB_OK ;
}

int ixmBucketManager::initialize ( const char *indexFile )
{
   int rc = EDB_OK ;
   ixmBucket *temp = NULL ;
   for ( int i = 0; i < IXM_HASH_MAP_SIZE; ++i )
   {
      temp = new (std::nothrow) ixmBucket () ;
      if ( !temp )
      {
         rc = EDB_OOM ;
         PD_LOG ( PDERROR, "Failed to allocate new ixmBucket" ) ;
         goto error ;
      }
      _bucket.push_back ( temp ) ;
      temp = NULL ;
   }
   if ( indexFile )
   {
      rc = loadFromDisk ( indexFile ) ;
      if ( EDB_OK != rc )
      {
         // No usable index yet: start a fresh log and let DMS rebuild the
         // buckets from the data file.
         FILE *file = fopen ( indexFile, "wb" ) ;
         if ( file )
         {
            unsigned int version = IXM_INDEX_VERSION ;
            fwrite ( IXM_INDEX_MAGIC, 1, 8, file ) ;
            fwrite ( &version, sizeof(version), 1, file ) ;
            fclose ( file ) ;
            _persistFile = fopen ( indexFile, "ab" ) ;
         }
         _persisted = false ;
         rc = EDB_OK ;
      }
   }
done:
   return rc ;
error :
   goto done ;
}
