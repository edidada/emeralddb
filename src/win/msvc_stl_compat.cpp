// Some MSVC toolsets reference these vectorized STL helpers from their
// headers but do not ship them in the CRT libraries. Provide weak fallbacks:
// they are used only when the CRT does not already define the symbol.
#ifdef _WIN32

extern "C" unsigned int __stdcall edb_find_first_not_of_pos(
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

extern "C" unsigned int __stdcall edb_find_last_not_of_pos(
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

#pragma comment(linker, "/alternatename:___std_find_first_not_of_trivial_pos_1@16=_edb_find_first_not_of_pos@16")
#pragma comment(linker, "/alternatename:___std_find_last_not_of_trivial_pos_1@16=_edb_find_last_not_of_pos@16")

#endif // _WIN32
