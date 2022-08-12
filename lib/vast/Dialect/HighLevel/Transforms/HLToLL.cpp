// Copyright (c) 2021-present, Trail of Bits, Inc.

#include "vast/Dialect/HighLevel/Passes.hpp"

VAST_RELAX_WARNINGS
#include <mlir/Analysis/DataLayoutAnalysis.h>
#include <mlir/IR/PatternMatch.h>
#include <mlir/Transforms/GreedyPatternRewriteDriver.h>
#include <mlir/Transforms/DialectConversion.h>
#include <mlir/Dialect/LLVMIR/LLVMDialect.h>
#include <mlir/Conversion/LLVMCommon/TypeConverter.h>
#include <mlir/Conversion/LLVMCommon/Pattern.h>
VAST_UNRELAX_WARNINGS

#include "PassesDetails.hpp"

#include "vast/Dialect/HighLevel/HighLevelAttributes.hpp"
#include "vast/Dialect/HighLevel/HighLevelTypes.hpp"
#include "vast/Dialect/HighLevel/HighLevelOps.hpp"
#include "vast/Util/TypeConverter.hpp"
#include "vast/Util/LLVMTypeConverter.hpp"
#include "vast/Util/Symbols.hpp"

#include <iostream>

namespace vast::hl
{
    using mctx_t = mlir::MLIRContext;

    namespace
    {
        std::size_t size(mlir::Region &region)
        {
            return std::distance(region.begin(), region.end());
        }

        std::size_t size(mlir::Block &block)
        {
            return std::distance(block.begin(), block.end());
        }

    }

    // TODO(lukas): In non-debug mode return `mlir::failure()` and do not log
    //              anything.
    #define VAST_PATTERN_CHECK(cond, fmt, ...) \
        VAST_CHECK(cond, fmt, __VA_ARGS__)

    namespace pattern
    {
        // NOTE(lukas): I would consider to just use the entire namespace, everything
        //              has (unfortunately) prefixed name with `LLVM` anyway.
        namespace LLVM = mlir::LLVM;

        using TypeConverter = util::tc::LLVMTypeConverter;

        template< typename O >
        struct BasePattern : mlir::ConvertOpToLLVMPattern< O >
        {
            using Base = mlir::ConvertOpToLLVMPattern< O >;
            using TC_t = util::TypeConverterWrapper< TypeConverter >;

            TypeConverter &tc;

            BasePattern(TypeConverter &tc_) : Base(tc_), tc(tc_) {}
            TypeConverter &type_converter() const { return tc; }
        };

        template< typename Src >
        struct ignore_pattern : BasePattern< Src >
        {
            using Base = BasePattern< Src >;
            using Base::Base;

            mlir::LogicalResult matchAndRewrite(
                        Src op, typename Src::Adaptor ops,
                        mlir::ConversionPatternRewriter &rewriter) const override
            {
                rewriter.replaceOp(op, ops.getOperands());
                return mlir::success();
            }
        };

        struct translation_unit : BasePattern< hl::TranslationUnitOp >
        {
            using Base = BasePattern< hl::TranslationUnitOp >;
            using Base::Base;

            mlir::LogicalResult matchAndRewrite(
                    hl::TranslationUnitOp unit_op, hl::TranslationUnitOp::Adaptor ops,
                    mlir::ConversionPatternRewriter &rewriter) const override
            {
                auto parent = unit_op.body().getParentRegion();
                rewriter.inlineRegionBefore(unit_op.body(), *parent, parent->end());

                // splice newly created translation unit block in the module
                auto &unit_block = parent->back();
                rewriter.mergeBlocks(&parent->front(), &unit_block, unit_block.getArguments());

                rewriter.eraseOp(unit_op);
                return mlir::success();
            }
        };

        struct func_op : BasePattern< mlir::FuncOp >
        {
            using Base = BasePattern< mlir::FuncOp >;
            using Base::Base;

            mlir::LogicalResult matchAndRewrite(
                    mlir::FuncOp func_op, mlir::FuncOp::Adaptor ops,
                    mlir::ConversionPatternRewriter &rewriter) const override
            {
                auto &tc = this->type_converter();
                auto maybe_target_type = tc.convert_fn_t(func_op.getType());
                auto maybe_signature =
                    tc.get_conversion_signature(func_op, util::tc::is_variadic(func_op));

                // Type converter failed.
                if (!maybe_target_type || !*maybe_target_type || !maybe_signature)
                    return mlir::failure();

                auto target_type = *maybe_target_type;
                auto signature = *maybe_signature;

                // TODO(lukas): We will want to lower a lot of stuff most likely.
                //              Copy those we want to preserve.
                mlir::SmallVector< mlir::NamedAttribute, 8 > new_attrs;

                if (auto original_arg_attr = func_op.getAllArgAttrs())
                {
                    mlir::SmallVector< mlir::Attribute, 8 > new_arg_attrs;
                    for (std::size_t i = 0; i < func_op.getNumArguments(); ++i)
                    {
                        const auto &mapping = signature.getInputMapping(i);
                        for (std::size_t j = 0; j < mapping->size; ++j)
                            new_arg_attrs[mapping->inputNo + j] = original_arg_attr[i];
                    }
                    new_attrs.push_back(rewriter.getNamedAttr(
                                mlir::FunctionOpInterface::getArgDictAttrName(),
                                rewriter.getArrayAttr(new_arg_attrs)));
                }
                // TODO(lukas): Linkage?
                auto linkage = LLVM::Linkage::External;
                auto new_func = rewriter.create< LLVM::LLVMFuncOp >(
                        func_op.getLoc(), func_op.getName(), target_type,
                        linkage, false, new_attrs);
                rewriter.inlineRegionBefore(func_op.getBody(),
                                            new_func.getBody(), new_func.end());
                util::convert_region_types(func_op, new_func, signature);

                if (mlir::failed(args_to_allocas(new_func, rewriter)))
                    return mlir::failure();
                rewriter.eraseOp(func_op);
                return mlir::success();
            }

            mlir::LogicalResult args_to_allocas(
                    mlir::LLVM::LLVMFuncOp fn,
                    mlir::ConversionPatternRewriter &rewriter) const
            {
                // TODO(lukas): Missing support in hl.
                if (fn.isVarArg())
                    return mlir::failure();

                if (fn.empty())
                    return mlir::failure();

                auto &block = fn.front();
                if (!block.isEntryBlock())
                    return mlir::failure();

                mlir::OpBuilder::InsertionGuard guard(rewriter);
                rewriter.setInsertionPointToStart(&block);

                for (auto arg : block.getArguments())
                    if (mlir::failed(arg_to_alloca(arg, block, rewriter)))
                        return mlir::failure();

                return mlir::success();
            }

            // TODO(lukas): Extract common codebase (there will be other places
            //              that need to create allocas).
            mlir::LogicalResult arg_to_alloca(mlir::BlockArgument arg, mlir::Block &block,
                                              mlir::ConversionPatternRewriter &rewriter) const
            {
                auto ptr_type = mlir::LLVM::LLVMPointerType::get(arg.getType());
                if (!ptr_type)
                    return mlir::failure();

                auto count = rewriter.create< LLVM::ConstantOp >(
                        arg.getLoc(),
                        type_converter().convertType(rewriter.getIndexType()),
                        rewriter.getIntegerAttr(rewriter.getIndexType(), 1));

                auto alloca_op = rewriter.create< LLVM::AllocaOp >(
                        arg.getLoc(), ptr_type, count, 0);

                arg.replaceAllUsesWith(alloca_op);
                rewriter.create< mlir::LLVM::StoreOp >(arg.getLoc(), arg, alloca_op);

                return mlir::success();
            }
        };

        struct constant_int : BasePattern< hl::ConstantIntOp >
        {
            using Base = BasePattern< hl::ConstantIntOp >;
            using Base::Base;

            mlir::LogicalResult matchAndRewrite(
                    hl::ConstantIntOp op, hl::ConstantIntOp::Adaptor ops,
                    mlir::ConversionPatternRewriter &rewriter) const override
            {
                rewriter.replaceOp(op, {make_from(op, rewriter, this->type_converter())});
                return mlir::success();
            }

            static LLVM::ConstantOp make_from(
                    hl::ConstantIntOp op,
                    mlir::ConversionPatternRewriter &rewriter,
                    auto &&tc)
            {
                auto src_ty = op.getType();
                auto target_ty = tc.convert_type_to_type(src_ty);

                return rewriter.create< LLVM::ConstantOp >(op.getLoc(), *target_ty,
                                                           op.getValue());
            }
        };

        struct ret : BasePattern< hl::ReturnOp >
        {
            using Base = BasePattern< hl::ReturnOp >;
            using Base::Base;

            mlir::LogicalResult matchAndRewrite(
                    hl::ReturnOp ret_op, hl::ReturnOp::Adaptor ops,
                    mlir::ConversionPatternRewriter &rewriter) const override
            {
                rewriter.create< LLVM::ReturnOp >(ret_op.getLoc(), ops.getOperands());
                rewriter.eraseOp(ret_op);
                return mlir::success();
            }
        };

        // Inline the region that is responsible for initialization
        //  * `rewriter` insert point is invalidated (although documentation of called
        //    methods does not state it, experimentally it is corrupted)
        //  * terminator is returned to be used & erased by caller.
        template< typename T >
        T inline_init_region(auto src, auto &rewriter)
        {
            VAST_ASSERT(size(src.initializer()) == 1);
            auto &init_region = src.initializer();
            auto &init_block = init_region.back();

            auto terminator = mlir::dyn_cast< T >(init_block.getTerminator());
            VAST_ASSERT(size(init_region) == 1 && terminator);
            rewriter.inlineRegionBefore(init_region, src->getBlock());
            auto ip = std::next(mlir::Block::iterator(src));
            VAST_ASSERT(ip != src->getBlock()->end());

            rewriter.mergeBlockBefore(&init_block, &*ip);
            return terminator;
        }

        struct var : BasePattern< hl::VarDecl >
        {
            using Base = BasePattern< hl::VarDecl >;
            using O = hl::VarDecl;
            using Base::Base;


            mlir::LogicalResult unfold_init(LLVM::AllocaOp alloca, hl::InitListExpr init,
                                            auto &rewriter) const
            {
                std::size_t i = 0;

                auto p_type = alloca.getType().cast< LLVM::LLVMPointerType >();
                VAST_PATTERN_CHECK(p_type, "Expected pointer.");
                auto a_type = p_type.getElementType().dyn_cast< LLVM::LLVMArrayType >();
                VAST_PATTERN_CHECK(a_type, "Expected array.");

                for (auto op : init.elements())
                {
                    auto e_type = LLVM::LLVMPointerType::get(a_type.getElementType());

                    auto index = rewriter.template create< LLVM::ConstantOp >(
                            op.getLoc(), type_converter().convertType(rewriter.getIndexType()),
                            rewriter.getIntegerAttr(rewriter.getIndexType(), i));
                    auto where = rewriter.template create< LLVM::GEPOp >(
                            alloca.getLoc(), e_type, alloca, index.getResult());
                    rewriter.template create< LLVM::StoreOp >(alloca.getLoc(), op, where);
                    ++i;
                }
                rewriter.eraseOp(init);
                return mlir::success();
            }

            mlir::LogicalResult make_init(LLVM::AllocaOp alloca, hl::ValueYieldOp yield,
                                          auto &rewriter) const
            {
                mlir::Value v = yield.getOperand();
                if (auto init_list = v.getDefiningOp< hl::InitListExpr >())
                    return unfold_init(alloca, init_list, rewriter);

                rewriter.template create< LLVM::StoreOp >(alloca.getLoc(), v, alloca);
                return mlir::success();
            }

            mlir::LogicalResult matchAndRewrite(
                    hl::VarDecl var_op, hl::VarDecl::Adaptor ops,
                    mlir::ConversionPatternRewriter &rewriter) const override
            {
                auto ptr_type = type_converter().convertType(var_op.getType());
                if (!ptr_type)
                    return mlir::failure();

                auto count = rewriter.create< LLVM::ConstantOp >(
                        var_op.getLoc(),
                        type_converter().convertType(rewriter.getIndexType()),
                        rewriter.getIntegerAttr(rewriter.getIndexType(), 1));
                auto alloca = rewriter.create< LLVM::AllocaOp >(
                        var_op.getLoc(), ptr_type, count, 0);

                auto yield = inline_init_region< hl::ValueYieldOp >(var_op, rewriter);
                rewriter.setInsertionPoint(yield);
                if (!mlir::succeeded(make_init(alloca, yield, rewriter)))
                    return mlir::failure();

                rewriter.eraseOp(yield);
                rewriter.replaceOp(var_op, {alloca});

                return mlir::success();
            }


        };

        // TODO(lukas): Move to some utils.
        auto create_trunc_or_sext(auto op, mlir::Type target, auto &rewriter,
                                  mlir::Location loc, const auto &dl)
            -> mlir::Value
        {
            VAST_ASSERT(op.getType().template isa< mlir::IntegerType >() &&
                        target.isa< mlir::IntegerType >());
            auto new_bw = dl.getTypeSizeInBits(target);
            auto original_bw = dl.getTypeSizeInBits(op.getType());

            if (new_bw == original_bw)
                return op;
            else if (new_bw > original_bw)
                return rewriter.template create< mlir::LLVM::SExtOp >(loc, target, op);
            else
                return rewriter.template create< mlir::LLVM::TruncOp >(loc, target, op);
        }

        struct implicit_cast : BasePattern< hl::ImplicitCastOp >
        {
            using Base = BasePattern< hl::ImplicitCastOp >;
            using Base::Base;

            mlir::LogicalResult matchAndRewrite(
                        hl::ImplicitCastOp op, hl::ImplicitCastOp::Adaptor ops,
                        mlir::ConversionPatternRewriter &rewriter) const override
            {
                if (op.kind() == hl::CastKind::LValueToRValue)
                {
                    // TODO(lukas): Without `--ccopts -xc` in case of `c = (x = 5)`
                    //              there will be a LValueToRValue cast on rvalue from
                    //              `(x = 5)` - not sure why that is so, so just fail
                    //              gracefully for now.
                    if (!op.getOperand().getType().isa< hl::LValueType >())
                        return mlir::failure();

                    auto loaded = rewriter.create< LLVM::LoadOp >(op.getLoc(),
                                                                  ops.getOperands()[0]);
                    rewriter.replaceOp(op, {loaded});
                    return mlir::success();
                }
                if (op.kind() == hl::CastKind::IntegralCast)
                {
                    const auto &dl = this->type_converter().getDataLayoutAnalysis()
                                                           ->getAtOrAbove(op);
                    auto coerced = create_trunc_or_sext(
                            ops.getOperands()[0], op.getType(),
                            rewriter, op.getLoc(), dl);
                    rewriter.replaceOp(op, {coerced});
                    return mlir::success();
                }
                return mlir::failure();
            }

        };

        template< typename Src, typename Trg >
        struct one_to_one : BasePattern< Src >
        {
            using Base = BasePattern< Src >;
            using Base::Base;

            mlir::LogicalResult matchAndRewrite(
                        Src op, typename Src::Adaptor ops,
                        mlir::ConversionPatternRewriter &rewriter) const override
            {
                auto target_ty = this->type_converter().convert_type_to_type(op.getType());
                auto new_op = rewriter.create< Trg >(op.getLoc(), *target_ty, ops.getOperands());
                rewriter.replaceOp(op, {new_op});
                return mlir::success();
            }
        };

        using add = one_to_one< hl::AddIOp, LLVM::AddOp >;
        using sub = one_to_one< hl::SubIOp, LLVM::SubOp >;

        template< typename Src, typename Trg >
        struct assign_pattern : BasePattern< Src >
        {
            using Base = BasePattern< Src >;
            using Base::Base;

            mlir::LogicalResult matchAndRewrite(
                        Src op, typename Src::Adaptor ops_,
                        mlir::ConversionPatternRewriter &rewriter) const override
            {
                auto ops = ops_.getOperands();
                auto alloca = ops[1];

                std::vector< mlir::Value > m_ops{ ops.begin(), ops.end() };

                // TODO(lukas): This should not happen?
                if (ops[0].getType().template isa< hl::LValueType >())
                    return mlir::failure();

                m_ops[0] = rewriter.create< LLVM::LoadOp >(op.getLoc(), alloca);
                auto target_ty = this->type_converter().convert_type_to_type(op.src().getType());
                // Probably the easiest way to compose this (some template specialization would
                // require a lot of boilerplate).
                auto new_op = [&]()
                {
                    if constexpr (!std::is_same_v< Trg, void >)
                        return rewriter.create< Trg >(op.getLoc(), *target_ty, m_ops);
                    else
                        return ops[0];
                }();

                rewriter.create< LLVM::StoreOp >(op.getLoc(), new_op, alloca);

                // `hl.assign` returns value for cases like `int x = y = 5;`
                rewriter.replaceOp(op, {new_op});
                return mlir::success();
            }
        };

        using assign_add = assign_pattern< hl::AddIAssignOp, LLVM::AddOp >;
        using assign_sub = assign_pattern< hl::SubIAssignOp, LLVM::SubOp >;
        using assign = assign_pattern< hl::AssignOp, void >;

        using declref = ignore_pattern< hl::DeclRefOp >;

        struct call : BasePattern< hl::CallOp >
        {
            using Base = BasePattern< hl::CallOp >;
            using Base::Base;

            mlir::LogicalResult matchAndRewrite(
                        hl::CallOp op, typename CallOp::Adaptor ops,
                        mlir::ConversionPatternRewriter &rewriter) const override
            {
                auto module = op->getParentOfType< mlir::ModuleOp >();
                if (!module)
                    return mlir::failure();

                auto callee = module.lookupSymbol< mlir::LLVM::LLVMFuncOp >(op.callee());
                if (!callee)
                    return mlir::failure();

                auto rtys = this->type_converter().convert_types_to_types(
                        callee.getResultTypes());
                if (!rtys)
                    return mlir::failure();

                auto new_call = rewriter.create< mlir::LLVM::CallOp >(
                    op.getLoc(),
                    *rtys,
                    op.callee(),
                    ops.getOperands());
                rewriter.replaceOp(op, new_call.getResults());
                return mlir::success();
            }
        };

        struct cmp : BasePattern< hl::CmpOp >
        {

            using Base = BasePattern< hl::CmpOp >;
            using Base::Base;

            mlir::LogicalResult matchAndRewrite(
                        hl::CmpOp op, typename CmpOp::Adaptor ops,
                        mlir::ConversionPatternRewriter &rewriter) const override
            {
                auto predicate = convert_predicate(op.predicate());
                if (!predicate)
                    return mlir::failure();

                auto new_cmp = rewriter.create< mlir::LLVM::ICmpOp >(
                    op.getLoc(), *predicate, ops.lhs(), ops.rhs());
                rewriter.replaceOp(op, { new_cmp });
                return mlir::success();
            }

            auto convert_predicate(auto hl_predicate) const
            -> std::optional< mlir::LLVM::ICmpPredicate >
            {
                // TODO(lukas): Use map later, this is just a proof of concept.
                switch (hl_predicate)
                {
                    case hl::Predicate::eq : return { mlir::LLVM::ICmpPredicate::eq };
                    case hl::Predicate::sgt : return { mlir::LLVM::ICmpPredicate::sgt };
                    default : return {};
                }
            }
        };

    } // namespace pattern


    struct HLToLLPass : HLToLLBase< HLToLLPass >
    {
        void runOnOperation() override;
    };

    void HLToLLPass::runOnOperation()
    {
        auto &mctx = this->getContext();
        mlir::ModuleOp op = this->getOperation();


        mlir::ConversionTarget target(mctx);
        target.addIllegalDialect< hl::HighLevelDialect >();
        target.addLegalOp< hl::TypeDefOp >();
        target.addIllegalOp< mlir::FuncOp >();
        target.markUnknownOpDynamicallyLegal([](auto) { return true; });

        const auto &dl_analysis = this->getAnalysis< mlir::DataLayoutAnalysis >();

        mlir::LowerToLLVMOptions llvm_options{ &mctx };
        llvm_options.useBarePtrCallConv = true;
        pattern::TypeConverter type_converter(&mctx, llvm_options , &dl_analysis);

        mlir::RewritePatternSet patterns(&mctx);
        patterns.add< pattern::translation_unit >(type_converter);
        patterns.add< pattern::func_op >(type_converter);
        patterns.add< pattern::var >(type_converter);
        patterns.add< pattern::constant_int >(type_converter);
        patterns.add< pattern::ret >(type_converter);
        patterns.add< pattern::add >(type_converter);
        patterns.add< pattern::sub >(type_converter);
        patterns.add< pattern::declref >(type_converter);
        patterns.add< pattern::assign_add >(type_converter);
        patterns.add< pattern::assign_sub >(type_converter);
        patterns.add< pattern::assign >(type_converter);
        patterns.add< pattern::implicit_cast >(type_converter);
        patterns.add< pattern::call >(type_converter);
        patterns.add< pattern::cmp >(type_converter);
        if (mlir::failed(mlir::applyPartialConversion(op, target, std::move(patterns))))
            return signalPassFailure();
    }
}


std::unique_ptr< mlir::Pass > vast::hl::createHLToLLPass()
{
    return std::make_unique< HLToLLPass >();
}
