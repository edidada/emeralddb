/*******************************************************************************
   Query predicates and plan selection.
*******************************************************************************/
#ifndef QRY_HPP__
#define QRY_HPP__

#include "bson.h"

namespace qry
{
   // True when the query has a usable _id equality predicate.
   bool hasIdLookup ( const bson::BSONObj &query ) ;

   // Evaluate a query predicate against a record.
   bool match ( const bson::BSONObj &record, const bson::BSONObj &query ) ;

   // Human readable plan name used by EXPLAIN.
   const char *planName ( const bson::BSONObj &query ) ;

   // Rough cost estimate: an _id lookup is constant, a scan is linear.
   int estimateCost ( const bson::BSONObj &query, unsigned int recordCount ) ;
}

#endif
