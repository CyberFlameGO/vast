# Copyright (c) 2022-present, Trail of Bits, Inc.

add_mlir_dialect_library(MLIRLowLevel
    LowLevelDialect.cpp
    LowLevelOps.cpp

    ADDITIONAL_HEADER_DIRS
    ${PROJECT_SOURCE_DIR}/include/vast

    DEPENDS
    MLIRLowLevelIncGen

    LINK_LIBS PUBLIC
    MLIRIR
    VASTSymbolInterface
)

target_link_libraries(MLIRLowLevel PRIVATE vast_settings)

add_subdirectory(Transforms)
