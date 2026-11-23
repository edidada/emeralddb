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
#include "pmd.hpp"
#include "pmdOptions.hpp"
#include "pd.hpp"

EDB_KRCB pmd_krcb ;
extern char _pdDiagLogPath [ OSS_MAX_PATHSIZE+1 ] ;
int EDB_KRCB::init ( pmdOptions *options )
{
   setDBStatus ( EDB_DB_NORMAL ) ;
   setDataFilePath ( options->getDBPath () ) ;
   setLogFilePath ( options->getLogPath () ) ;
   strncpy ( _pdDiagLogPath, getLogFilePath(), sizeof(_pdDiagLogPath) ) ;
   setSvcName ( options->getServiceName () ) ;
   setMaxPool ( options->getMaxPool () ) ;
   int rc = _rtnMgr.rtnInitialize ( getDataFilePath() ) ;
   if ( rc )
   {
      return rc ;
   }
   std::string walPath = std::string ( getDataFilePath() ) + ".wal" ;
   rc = _txnMgr.initialize ( walPath.c_str() ) ;
   if ( rc )
   {
      return rc ;
   }
   rc = _txnMgr.recover ( &_rtnMgr ) ;
   if ( rc )
   {
      return rc ;
   }
   int role = 0 == strcmp ( options->getRole(), "secondary" ) ?
                 REPL_ROLE_SECONDARY : REPL_ROLE_PRIMARY ;
   rc = _replMgr.initialize ( role, options->getPeer(),
                              atoi ( getSvcName() ),
                              getDataFilePath(), &_rtnMgr ) ;
   if ( rc )
   {
      return rc ;
   }
   return _replMgr.start () ;
}

