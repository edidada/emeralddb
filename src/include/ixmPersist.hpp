/*******************************************************************************
   On-disk format for the persistent _id hash index.

   The file is an append-only log. A fixed header is followed by a sequence of
   entries. Each entry stores the bucket, the hash, the record location and the
   raw BSON element bytes of the _id, so the in-memory multimap can be rebuilt
   without scanning the data file.
*******************************************************************************/
#ifndef IXMPERSIST_HPP__
#define IXMPERSIST_HPP__

#define IXM_INDEX_MAGIC "EDBIDX01"
#define IXM_INDEX_VERSION 1

#define IXM_IDX_OP_INSERT 1
#define IXM_IDX_OP_DELETE 2

#endif
