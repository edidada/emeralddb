/*******************************************************************************
   Multi-version store implementation.
*******************************************************************************/
#include "mvs.hpp"
#include "pd.hpp"

std::string mvsKeyOf ( const bson::BSONObj &record )
{
   bson::BSONElement id = record.getField ( "_id" ) ;
   if ( id.eoo() )
   {
      return std::string () ;
   }
   return id.toString() ;
}

int mvsManager::initialize ( const char *path )
{
   if ( !path )
   {
      return EDB_INVALIDARG ;
   }
   _path = path ;
   int rc = load () ;
   if ( rc )
   {
      // no usable snapshot: start from an empty version store
      _store.clear () ;
      _clock = 0 ;
      rc = save () ;
   }
   return rc ;
}

int mvsManager::load ()
{
   FILE *file = fopen ( _path.c_str(), "rb" ) ;
   if ( !file )
   {
      return EDB_IO ;
   }
   char magic[9] = {0} ;
   unsigned int version = 0 ;
   if ( fread ( magic, 1, 8, file ) != 8 ||
        strcmp ( magic, MVS_MAGIC ) ||
        fread ( &version, sizeof(version), 1, file ) != 1 ||
        version != MVS_VERSION )
   {
      fclose ( file ) ;
      return EDB_INVALIDARG ;
   }
   _store.clear () ;
   _clock = 0 ;
   while ( true )
   {
      unsigned int keyLen = 0, dataLen = 0 ;
      uint64_t beginTS = 0, endTS = 0 ;
      unsigned char deleted = 0 ;
      if ( fread ( &keyLen, sizeof(keyLen), 1, file ) != 1 )
      {
         break ;
      }
      if ( fread ( &beginTS, sizeof(beginTS), 1, file ) != 1 ||
           fread ( &endTS, sizeof(endTS), 1, file ) != 1 ||
           fread ( &deleted, sizeof(deleted), 1, file ) != 1 ||
           fread ( &dataLen, sizeof(dataLen), 1, file ) != 1 ||
           keyLen == 0 || keyLen > 4096 || dataLen > 16 * 1024 * 1024 )
      {
         fclose ( file ) ;
         return EDB_INVALIDARG ;
      }
      std::vector<char> keyBuf ( keyLen ) ;
      std::vector<char> dataBuf ( dataLen > 0 ? dataLen : 1 ) ;
      if ( fread ( &keyBuf[0], 1, keyLen, file ) != keyLen ||
           ( dataLen > 0 && fread ( &dataBuf[0], 1, dataLen, file ) != dataLen ) )
      {
         fclose ( file ) ;
         return EDB_INVALIDARG ;
      }
      mvsVersion ver ;
      ver.beginTS = beginTS ;
      ver.endTS   = endTS ;
      ver.deleted = deleted != 0 ;
      ver.data.assign ( &dataBuf[0], dataLen ) ;
      _store[std::string ( &keyBuf[0], keyLen )].push_back ( ver ) ;
      if ( beginTS > _clock )
      {
         _clock = beginTS ;
      }
   }
   fclose ( file ) ;
   return EDB_OK ;
}

int mvsManager::save ()
{
   std::string tmp = _path + ".tmp" ;
   FILE *file = fopen ( tmp.c_str(), "wb" ) ;
   if ( !file )
   {
      return EDB_IO ;
   }
   unsigned int version = MVS_VERSION ;
   fwrite ( MVS_MAGIC, 1, 8, file ) ;
   fwrite ( &version, sizeof(version), 1, file ) ;
   for ( std::map<std::string, std::vector<mvsVersion> >::iterator it =
            _store.begin() ; it != _store.end() ; ++it )
   {
      for ( size_t i = 0 ; i < it->second.size() ; ++i )
      {
         mvsVersion &ver = it->second[i] ;
         unsigned int keyLen = (unsigned int)it->first.size() ;
         unsigned int dataLen = (unsigned int)ver.data.size() ;
         unsigned char deleted = ver.deleted ? 1 : 0 ;
         fwrite ( &keyLen, sizeof(keyLen), 1, file ) ;
         fwrite ( &ver.beginTS, sizeof(ver.beginTS), 1, file ) ;
         fwrite ( &ver.endTS, sizeof(ver.endTS), 1, file ) ;
         fwrite ( &deleted, sizeof(deleted), 1, file ) ;
         fwrite ( &dataLen, sizeof(dataLen), 1, file ) ;
         fwrite ( it->first.data(), 1, keyLen, file ) ;
         fwrite ( ver.data.data(), 1, dataLen, file ) ;
      }
   }
   fflush ( file ) ;
   fclose ( file ) ;
   remove ( _path.c_str() ) ;
   if ( rename ( tmp.c_str(), _path.c_str() ) )
   {
      return EDB_IO ;
   }
   return EDB_OK ;
}

uint64_t mvsManager::nextTS ()
{
   _mutex.get () ;
   ++_clock ;
   uint64_t ts = _clock ;
   _mutex.release () ;
   return ts ;
}

uint64_t mvsManager::currentTS ()
{
   _mutex.get () ;
   uint64_t ts = _clock ;
   _mutex.release () ;
   return ts ;
}

int mvsManager::visible ( const std::string &key, uint64_t snapshot,
                          std::string &data )
{
   if ( key.empty() )
   {
      return EDB_INVALIDARG ;
   }
   _mutex.get_shared () ;
   int rc = EDB_IXM_ID_NOT_EXIST ;
   std::map<std::string, std::vector<mvsVersion> >::iterator it =
      _store.find ( key ) ;
   if ( it != _store.end() )
   {
      std::vector<mvsVersion> &versions = it->second ;
      for ( int i = (int)versions.size() - 1 ; i >= 0 ; --i )
      {
         if ( versions[i].beginTS <= snapshot && snapshot < versions[i].endTS )
         {
            if ( !versions[i].deleted )
            {
               data = versions[i].data ;
               rc = EDB_OK ;
            }
            break ;
         }
      }
   }
   _mutex.release_shared () ;
   return rc ;
}

int mvsManager::checkConflicts ( const std::vector<mvsWriteOp> &writes,
                                 uint64_t snapshot )
{
   _mutex.get_shared () ;
   int rc = EDB_OK ;
   for ( size_t i = 0 ; i < writes.size() ; ++i )
   {
      std::map<std::string, std::vector<mvsVersion> >::iterator it =
         _store.find ( writes[i].key ) ;
      if ( it == _store.end() )
      {
         continue ;
      }
      for ( size_t j = 0 ; j < it->second.size() ; ++j )
      {
         if ( it->second[j].beginTS > snapshot )
         {
            rc = EDB_CONFLICT ;
            break ;
         }
      }
      if ( rc )
      {
         break ;
      }
   }
   _mutex.release_shared () ;
   return rc ;
}

int mvsManager::commit ( const std::vector<mvsWriteOp> &writes, uint64_t ts )
{
   _mutex.get () ;
   for ( size_t i = 0 ; i < writes.size() ; ++i )
   {
      std::vector<mvsVersion> &versions = _store[writes[i].key] ;
      for ( size_t j = 0 ; j < versions.size() ; ++j )
      {
         if ( versions[j].endTS == MVS_MAX_TS )
         {
            versions[j].endTS = ts ;
         }
      }
      mvsVersion ver ;
      ver.beginTS = ts ;
      ver.endTS   = MVS_MAX_TS ;
      ver.deleted = ( writes[i].op == 2 ) ;
      ver.data    = writes[i].data ;
      versions.push_back ( ver ) ;
   }
   if ( ts > _clock )
   {
      _clock = ts ;
   }
   int rc = save () ;
   _mutex.release () ;
   return rc ;
}

int mvsManager::getAll ( std::vector<std::string> &records )
{
   _mutex.get_shared () ;
   for ( std::map<std::string, std::vector<mvsVersion> >::iterator it =
            _store.begin() ; it != _store.end() ; ++it )
   {
      std::vector<mvsVersion> &versions = it->second ;
      for ( int i = (int)versions.size() - 1 ; i >= 0 ; --i )
      {
         if ( versions[i].endTS == MVS_MAX_TS && !versions[i].deleted )
         {
            records.push_back ( versions[i].data ) ;
            break ;
         }
      }
   }
   _mutex.release_shared () ;
   return EDB_OK ;
}
