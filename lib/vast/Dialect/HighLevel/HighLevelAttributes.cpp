// Copyright (c) 2021-present, Trail of Bits, Inc.

#include "vast/Dialect/HighLevel/HighLevelAttributes.hpp"

#define GET_ATTRDEF_CLASSES
#include "vast/Dialect/HighLevel/HighLevelAttributes.cpp.inc"

namespace vast::hl
{
  void HighLevelDialect::registerAttributes()
  {
    addAttributes<
        #define GET_ATTRDEF_LIST
        #include "vast/Dialect/HighLevel/HighLevelAttributes.cpp.inc"
    >();
  }

} // namespace vast::hl