/*******************************************************************************
   Query predicate evaluation and cost based plan selection.
*******************************************************************************/
#include "qry.hpp"

namespace qry
{

static int compare ( const bson::BSONElement &lhs, const bson::BSONElement &rhs )
{
   if ( lhs.isNumber() && rhs.isNumber() )
   {
      double a = lhs.number() ;
      double b = rhs.number() ;
      return a < b ? -1 : ( a > b ? 1 : 0 ) ;
   }
   if ( lhs.type() == bson::String && rhs.type() == bson::String )
   {
      return lhs.str().compare ( rhs.str() ) ;
   }
   if ( lhs.type() == bson::Bool && rhs.type() == bson::Bool )
   {
      bool a = lhs.Bool() ;
      bool b = rhs.Bool() ;
      return a == b ? 0 : ( a ? 1 : -1 ) ;
   }
   // different or unsupported types: compare their canonical strings
   return lhs.toString().compare ( rhs.toString() ) ;
}

bool hasIdLookup ( const bson::BSONObj &query )
{
   bson::BSONElement id = query.getField ( "_id" ) ;
   return !id.eoo() && id.type() != bson::Object ;
}

static bool matchOperator ( const bson::BSONElement &value,
                            const bson::BSONElement &op )
{
   const char *name = op.fieldName() ;
   int cmp = compare ( value, op ) ;
   if ( !strcmp ( name, "$eq" ) )
   {
      return cmp == 0 ;
   }
   if ( !strcmp ( name, "$ne" ) )
   {
      return cmp != 0 ;
   }
   if ( !strcmp ( name, "$gt" ) )
   {
      return cmp > 0 ;
   }
   if ( !strcmp ( name, "$gte" ) )
   {
      return cmp >= 0 ;
   }
   if ( !strcmp ( name, "$lt" ) )
   {
      return cmp < 0 ;
   }
   if ( !strcmp ( name, "$lte" ) )
   {
      return cmp <= 0 ;
   }
   // unknown operator: treat as equality
   return cmp == 0 ;
}

bool match ( const bson::BSONObj &record, const bson::BSONObj &query )
{
   bson::BSONObjIterator it ( query ) ;
   while ( it.more() )
   {
      bson::BSONElement qele = *it ;
      ++it ;
      bson::BSONElement rele = record.getField ( qele.fieldName() ) ;
      if ( rele.eoo() )
      {
         return false ;
      }
      if ( qele.type() == bson::Object )
      {
         bson::BSONObj ops = qele.embeddedObject() ;
         bson::BSONObjIterator oit ( ops ) ;
         while ( oit.more() )
         {
            bson::BSONElement op = *oit ;
            ++oit ;
            if ( !matchOperator ( rele, op ) )
            {
               return false ;
            }
         }
      }
      else if ( compare ( rele, qele ) != 0 )
      {
         return false ;
      }
   }
   return true ;
}

const char *planName ( const bson::BSONObj &query )
{
   return hasIdLookup ( query ) ? "ID_INDEX_LOOKUP" : "TABLE_SCAN" ;
}

int estimateCost ( const bson::BSONObj &query, unsigned int recordCount )
{
   if ( hasIdLookup ( query ) )
   {
      return 1 ;
   }
   return (int)recordCount > 0 ? (int)recordCount : 1 ;
}

}
