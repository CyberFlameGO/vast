add_mlir_dialect_library(MLIRHighLevelTransforms
  ExportFnInfo.cpp
  HLLowerTypes.cpp
  HLStructsToLLVM.cpp
  HLToSCF.cpp
  LLVMDump.cpp
  HLToLLGEPs.cpp
  HLToLLVars.cpp

  DEPENDS
  HighLevelTransformsIncGen

  LINK_LIBS PUBLIC
  MLIRLowLevel
  MLIRHighLevel
  MLIRIR
  MLIRPass
  MLIRTransformUtils
  MLIRExecutionEngine
  MLIRLLVMCommonConversion
  MLIRTargetLLVMIRExport
  MLIRLLVMToLLVMIRTranslation
  MLIRToLLVMIRTranslationRegistration
)
