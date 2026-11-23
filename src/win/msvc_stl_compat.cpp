// Workaround for recent MSVC toolsets where the vectorized first/last
// "not of" helpers are referenced by the STL headers but not provided by
// the shipped CRT libraries. Only needed for 32-bit Windows builds.
#ifdef _WIN32

extern "C" {

unsigned int __stdcall __std_find_first_not_of_trivial_pos_1(
   const char *first, unsigned int count,
   const char *match, unsigned int matchCount)
{
   for ( unsigned int i = 0 ; i < count ; ++i )
   {
      bool found = false ;
      for ( unsigned int j = 0 ; j < matchCount ; ++j )
      {
         if ( first[i] == match[j] )
         {
            found = true ;
            break ;
         }
      }
      if ( !found )
      {
         return i ;
      }
   }
   return count ;
}

unsigned int __stdcall __std_find_last_not_of_trivial_pos_1(
   const char *first, unsigned int count,
   const char *match, unsigned int matchCount)
{
   for ( unsigned int i = count ; i > 0 ; --i )
   {
      bool found = false ;
      for ( unsigned int j = 0 ; j < matchCount ; ++j )
      {
         if ( first[i - 1] == match[j] )
         {
            found = true ;
            break ;
         }
      }
      if ( !found )
      {
         return i - 1 ;
      }
   }
   return count ;
}

} // extern "C"

#endif // _WIN32
