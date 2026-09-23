add_executable(gravtree_bench bench.cpp)
target_link_libraries(gravtree_bench gravtree_lib benchmark::benchmark_main)

# ── Latency benchmark (GravTree vs RocksDB) ──────────────────────────────────
find_library(ROCKSDB_LIB rocksdb
    HINTS /opt/homebrew/lib /usr/local/lib
    REQUIRED
)
find_path(ROCKSDB_INCLUDE rocksdb/db.h
    HINTS /opt/homebrew/include /usr/local/include
    REQUIRED
)

add_executable(gravtree_latency latency_bench.cpp)
# RocksDB >= 11.8 public headers require C++20 (`using enum`).
set_target_properties(gravtree_latency PROPERTIES CXX_STANDARD 20)
target_include_directories(gravtree_latency PRIVATE ${ROCKSDB_INCLUDE})
target_link_libraries(gravtree_latency gravtree_lib ${ROCKSDB_LIB})

add_executable(gravtree_verify verify_bench.cpp)
target_link_libraries(gravtree_verify gravtree_lib)

add_executable(gravtree_wa wa_bench.cpp)
set_target_properties(gravtree_wa PROPERTIES CXX_STANDARD 20)
target_include_directories(gravtree_wa PRIVATE ${ROCKSDB_INCLUDE})
target_link_libraries(gravtree_wa gravtree_lib ${ROCKSDB_LIB})
