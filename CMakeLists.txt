cmake_minimum_required(VERSION 3.15)
project(emeralddb CXX)

# The legacy sources use `_WINDOWS` (not the `_WIN32` that MSVC defines
# automatically) to select the Windows code paths.
if(WIN32)
   add_compile_definitions(_WINDOWS WIN32 _CRT_SECURE_NO_WARNINGS BOOST_ALL_NO_LIB)
endif()

set(SRC_DIR "${CMAKE_CURRENT_SOURCE_DIR}/src")
set(INC_DIRS
   "${SRC_DIR}"
   "${SRC_DIR}/include"
   "${SRC_DIR}/bson/src"
   "${CMAKE_CURRENT_SOURCE_DIR}/boost"
)

# Boost 1.49 is built by CI into boost/stage/lib. Auto-linking is disabled
# above, so link the produced archives directly.
file(GLOB BOOST_LIBS
   "${CMAKE_CURRENT_SOURCE_DIR}/boost/stage/lib/*.lib"
   "${CMAKE_CURRENT_SOURCE_DIR}/boost/stage/lib/*.a")

set(BSON_SOURCES
   "${SRC_DIR}/bson/src/bsonobj.cpp"
   "${SRC_DIR}/bson/src/oid.cpp"
   "${SRC_DIR}/bson/src/util/json.cpp"
   "${SRC_DIR}/bson/src/lib/base64.cpp"
   "${SRC_DIR}/bson/src/lib/md5.cpp"
   "${SRC_DIR}/bson/src/lib/nonce.cpp"
)

set(WIN_COMMON_SOURCES
   "${SRC_DIR}/win/emeralddb_win.cpp"
   "${SRC_DIR}/win/msvc_stl_compat.cpp"
)

add_executable(emeralddb
   ${BSON_SOURCES}
   ${WIN_COMMON_SOURCES}
   "${SRC_DIR}/msg/msg.cpp"
   "${SRC_DIR}/pd/pd.cpp"
   "${SRC_DIR}/dms/dms.cpp"
   "${SRC_DIR}/ixm/ixmBucket.cpp"
   "${SRC_DIR}/oss/ossHash.cpp"
   "${SRC_DIR}/oss/ossMmapFile.cpp"
   "${SRC_DIR}/oss/ossPrimitiveFileOp.cpp"
   "${SRC_DIR}/oss/ossSocket.cpp"
   "${SRC_DIR}/pmd/pmd.cpp"
   "${SRC_DIR}/pmd/pmdAgent.cpp"
   "${SRC_DIR}/pmd/pmdEDU.cpp"
   "${SRC_DIR}/pmd/pmdEDUMgr.cpp"
   "${SRC_DIR}/pmd/pmdMain.cpp"
   "${SRC_DIR}/pmd/pmdOptions.cpp"
   "${SRC_DIR}/pmd/pmdTcpListener.cpp"
   "${SRC_DIR}/rtn/rtn.cpp"
   "${SRC_DIR}/txn/txn.cpp"
)

add_executable(client
   ${BSON_SOURCES}
   ${WIN_COMMON_SOURCES}
   "${SRC_DIR}/client/command.cpp"
   "${SRC_DIR}/client/commandFactory.cpp"
   "${SRC_DIR}/client/edb.cpp"
   "${SRC_DIR}/msg/msg.cpp"
   "${SRC_DIR}/pd/pd.cpp"
   "${SRC_DIR}/oss/ossPrimitiveFileOp.cpp"
   "${SRC_DIR}/oss/ossSocket.cpp"
)

foreach(target emeralddb client)
   target_include_directories(${target} PRIVATE ${INC_DIRS})
   target_link_libraries(${target} PRIVATE ${BOOST_LIBS})
   if(WIN32)
      target_link_libraries(${target} PRIVATE ws2_32)
   else()
      find_package(Threads REQUIRED)
      target_link_libraries(${target} PRIVATE Threads::Threads)
   endif()
endforeach()
