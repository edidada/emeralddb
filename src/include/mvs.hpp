/*******************************************************************************
   Multi-version store providing snapshot isolation.

   Every record key keeps a chain of versions with [beginTS, endTS) validity.
   Readers see the version visible at their snapshot; writers create a new
   version at commit time after verifying that no newer version exists.
   The store is persisted as a snapshot file and rewritten atomically on commit.
*******************************************************************************/
#ifndef MVS_HPP__
#define MVS_HPP__

#include <map>
#include <string>
#include <vector>
#include <stdint.h>
#include "core.hpp"
#include "ossLatch.hpp"
#include "bson.h"

#define MVS_MAGIC "EDBMVS01"
#define MVS_VERSION 1
#define MVS_MAX_TS UINT64_MAX

struct mvsVersion
{
   uint64_t beginTS ;
   uint64_t endTS ;
   bool deleted ;
   std::string data ;
} ;

struct mvsWriteOp
{
   std::string key ;
   int op ; // 1 = upsert, 2 = delete
   std::string data ;
} ;

std::string mvsKeyOf ( const bson::BSONObj &record ) ;

class mvsManager
{
private :
   std::map<std::string, std::vector<mvsVersion> > _store ;
   uint64_t _clock ;
   std::string _path ;
   ossSLatch _mutex ;

   int load () ;
   int save () ;
public :
   mvsManager () : _clock(0) {}
   int initialize ( const char *path ) ;
   uint64_t nextTS () ;
   uint64_t currentTS () ;
   int visible ( const std::string &key, uint64_t snapshot,
                 std::string &data ) ;
   int checkConflicts ( const std::vector<mvsWriteOp> &writes,
                        uint64_t snapshot ) ;
   int commit ( const std::vector<mvsWriteOp> &writes, uint64_t ts ) ;
   int getAll ( std::vector<std::string> &records ) ;
} ;

#endif
