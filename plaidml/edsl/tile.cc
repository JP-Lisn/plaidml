#include "plaidml/edsl/tile.h"

#include <algorithm>
#include <iterator>

#include <boost/format.hpp>
#include <boost/optional.hpp>

#include "base/util/logging.h"
#include "tile/lang/ast.h"

namespace vertexai {
namespace plaidml {
namespace edsl {

namespace ast = tile::lang;
using Polynomial = tile::math::Polynomial<tile::math::Rational>;

struct TensorIndex::Impl {
  std::shared_ptr<ast::PolyExpr> expr;
};

struct TensorDim::Impl {
  boost::optional<size_t> size;
};

struct IndexedTensor::Impl {
  std::shared_ptr<ast::Expr> expr;
  Tensor::Impl* src = nullptr;
  void MakeContraction(ast::AggregationOp agg_op, const IndexedTensor& rhs);
  IndexedTensor MakeCall(const std::string& fn, const IndexedTensor& rhs);
};

struct Tensor::Impl {
  std::shared_ptr<ast::Expr> expr;
  std::vector<TensorDim> dims;
  std::string name;
};

class TensorFriend {
 public:
  static TensorIndex MakePolyOp(ast::PolyOp op, const std::vector<TensorIndex>& args) {
    auto impl = std::make_unique<TensorIndex::Impl>();
    std::vector<std::shared_ptr<ast::PolyExpr>> operands;
    for (const auto& arg : args) {
      operands.push_back(arg.impl_->expr);
    }
    impl->expr = std::make_shared<ast::PolyOpExpr>(op, operands);
    IVLOG(2, "MakePolyOp> " << impl->expr->str());
    return TensorIndex{std::move(impl)};
  }

  static TensorIndex MakeMixedPolyBinaryOp(ast::PolyOp op, const TensorIndex& idx, const TensorDim& dim,
                                           bool lhs_first) {
    if (!dim.impl_->size) {
      throw std::runtime_error("Undefined dimension.");
    }
    auto impl = std::make_unique<TensorIndex::Impl>();
    std::vector<std::shared_ptr<ast::PolyExpr>> operands;
    if (lhs_first) {
      operands.emplace_back(idx.impl_->expr);
      operands.emplace_back(std::make_shared<ast::PolyLiteral>(*dim.impl_->size));
    } else {
      operands.emplace_back(std::make_shared<ast::PolyLiteral>(*dim.impl_->size));
      operands.emplace_back(idx.impl_->expr);
    }
    impl->expr = std::make_shared<ast::PolyOpExpr>(op, operands);
    IVLOG(2, "MakeMixedPolyBinaryOp> " << impl->expr->str());
    return TensorIndex{std::move(impl)};
  }

  static TensorDim DimOp(ast::PolyOp op, const std::vector<TensorDim>& args) {
    std::vector<size_t> sizes;
    for (const auto& arg : args) {
      if (!arg.impl_->size) {
        throw std::runtime_error("Undefined dimension.");
      }
      sizes.emplace_back(*arg.impl_->size);
    }
    switch (op) {
      case ast::PolyOp::Neg:
        return TensorDim{-sizes[0]};
      case ast::PolyOp::Add:
        return TensorDim{sizes[0] + sizes[1]};
      case ast::PolyOp::Sub:
        return TensorDim{sizes[0] - sizes[1]};
      case ast::PolyOp::Mul:
        return TensorDim{sizes[0] * sizes[1]};
      case ast::PolyOp::Div:
        return TensorDim{sizes[0] / sizes[1]};
      default:
        throw std::runtime_error("Invalid poly op");
    }
  }

  inline static const TensorIndex::Impl* GetImpl(const ast::PolyIndex& expr) {
    return static_cast<const TensorIndex::Impl*>(expr.ptr);
  }

  inline static const Tensor::Impl* GetImpl(const Tensor& tensor) { return tensor.impl_.get(); }

  static IndexedTensor cond(const IndexedTensor& lhs, const IndexedTensor& rhs, const IndexedTensor& true_case) {
    auto impl = std::make_unique<IndexedTensor::Impl>();
    std::vector<std::shared_ptr<ast::Expr>> args = {lhs.impl_->expr, rhs.impl_->expr, true_case.impl_->expr};
    impl->expr = std::make_shared<ast::CallExpr>("cond", args);
    return IndexedTensor{std::move(impl)};
  }

  static Tensor Call(const std::string& fn, const std::vector<Tensor>& args) {
    auto impl = std::make_unique<Tensor::Impl>();
    std::vector<std::shared_ptr<ast::Expr>> exprs;
    for (const auto& tensor : args) {
      exprs.push_back(tensor.impl_->expr);
    }
    impl->expr = std::make_shared<ast::CallExpr>(fn, exprs);
    return Tensor{std::move(impl)};
  }
};

TensorIndex::TensorIndex() : impl_(std::make_shared<Impl>()) {
  impl_->expr = std::make_shared<ast::PolyIndex>(impl_.get());
}

TensorIndex::TensorIndex(size_t value) : impl_(std::make_shared<Impl>()) {
  impl_->expr = std::make_shared<ast::PolyLiteral>(value);
}

TensorIndex::TensorIndex(const std::string& name) : impl_(std::make_shared<Impl>()) {
  impl_->expr = std::make_shared<ast::PolyIndex>(impl_.get(), name);
}

TensorIndex::TensorIndex(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}

TensorIndex TensorIndex::operator-() const { return TensorFriend::MakePolyOp(ast::PolyOp::Neg, {*this}); }

#define TILE_CC_DEFINE_TENSOR_IDXDIM_BINARY_OPS(_op_, _poly_op_)              \
  TensorIndex operator _op_(const TensorIndex& lhs, const TensorIndex& rhs) { \
    return TensorFriend::MakePolyOp(_poly_op_, {lhs, rhs});                   \
  }                                                                           \
  TensorIndex operator _op_(const TensorIndex& lhs, size_t rhs) {             \
    return TensorFriend::MakePolyOp(_poly_op_, {lhs, TensorIndex{rhs}});      \
  }                                                                           \
  TensorIndex operator _op_(size_t lhs, const TensorIndex& rhs) {             \
    return TensorFriend::MakePolyOp(_poly_op_, {TensorIndex{lhs}, rhs});      \
  }                                                                           \
  TensorIndex operator _op_(const TensorIndex& lhs, const TensorDim& rhs) {   \
    return TensorFriend::MakeMixedPolyBinaryOp(_poly_op_, lhs, rhs, true);    \
  }                                                                           \
  TensorIndex operator _op_(const TensorDim& lhs, const TensorIndex& rhs) {   \
    return TensorFriend::MakeMixedPolyBinaryOp(_poly_op_, rhs, lhs, false);   \
  }                                                                           \
  TensorDim operator _op_(const TensorDim& lhs, const TensorDim& rhs) {       \
    return TensorFriend::DimOp(_poly_op_, {lhs, rhs});                        \
  }                                                                           \
  TensorDim operator _op_(size_t lhs, const TensorDim& rhs) {                 \
    return TensorFriend::DimOp(_poly_op_, {TensorDim{lhs}, rhs});             \
  }                                                                           \
  TensorDim operator _op_(const TensorDim& lhs, size_t rhs) {                 \
    return TensorFriend::DimOp(_poly_op_, {lhs, TensorDim{rhs}});             \
  }

TILE_CC_DEFINE_TENSOR_IDXDIM_BINARY_OPS(+, ast::PolyOp::Add);
TILE_CC_DEFINE_TENSOR_IDXDIM_BINARY_OPS(-, ast::PolyOp::Sub);
TILE_CC_DEFINE_TENSOR_IDXDIM_BINARY_OPS(*, ast::PolyOp::Mul);
TILE_CC_DEFINE_TENSOR_IDXDIM_BINARY_OPS(/, ast::PolyOp::Div);

Constraint TensorIndex::operator<(size_t rhs) const {
  IVLOG(3, "Add size_t constraint: " << rhs);
  auto constraint = std::make_shared<ast::ConstraintExpr>(impl_->expr, rhs);
  ast::ConstraintApplier applier(constraint);
  impl_->expr->Accept(&applier);
  return Constraint();
}

Constraint TensorIndex::operator<(const TensorDim& rhs) const {
  if (!rhs.impl_->size) {
    throw std::runtime_error("Undefined dimension.");
  }
  IVLOG(3, "Add TensorDim constraint: " << *rhs.impl_->size);
  auto constraint = std::make_shared<ast::ConstraintExpr>(impl_->expr, *rhs.impl_->size);
  ast::ConstraintApplier applier(constraint);
  impl_->expr->Accept(&applier);
  return Constraint();
}

TensorDim::TensorDim() : impl_(std::make_shared<Impl>()) {}

TensorDim::TensorDim(size_t value) : impl_(std::make_shared<Impl>()) { impl_->size = value; }

Tensor::Tensor() : impl_(new Impl) { impl_->expr = std::make_shared<ast::ParamExpr>(tile::TensorShape{}, ""); }

Tensor::Tensor(const std::string& name) : impl_(new Impl) {
  impl_->expr = std::make_shared<ast::ParamExpr>(tile::TensorShape{}, name);
}

Tensor::Tensor(const tile::TensorShape& shape) : impl_(new Impl) {
  impl_->expr = std::make_shared<ast::ParamExpr>(shape, "");
}

Tensor::Tensor(const std::string& name, const tile::TensorShape& shape) : impl_(new Impl) {
  impl_->expr = std::make_shared<ast::ParamExpr>(shape, name);
}

Tensor::Tensor(const std::initializer_list<TensorDim>& dims) : impl_(new Impl) { impl_->dims = dims; }

Tensor::Tensor(const std::vector<TensorDim>& dims) : impl_(new Impl) { impl_->dims = dims; }

Tensor::Tensor(const std::string& name, const std::vector<TensorDim>& dims) : impl_(new Impl) {
  impl_->dims = dims;
  impl_->name = name;
}

Tensor::Tensor(const std::string& name, const std::initializer_list<TensorDim>& dims) : impl_(new Impl) {
  impl_->dims = dims;
  impl_->name = name;
}

Tensor::Tensor(int value) : impl_(new Impl) { impl_->expr = std::make_shared<ast::IntConst>(value); }

Tensor::Tensor(int64_t value) : impl_(new Impl) { impl_->expr = std::make_shared<ast::IntConst>(value); }

Tensor::Tensor(double value) : impl_(new Impl) { impl_->expr = std::make_shared<ast::FloatConst>(value); }

Tensor::~Tensor() = default;

// Impl Constructor
Tensor::Tensor(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}

// Copy Constructor
Tensor::Tensor(const Tensor& rhs) { *this = rhs; }

// Copy Assignment
Tensor& Tensor::operator=(const Tensor& rhs) {
  if (this != &rhs) {
    impl_.reset(new Impl(*rhs.impl_));
  }
  return *this;
}

void Tensor::bind_dims(const std::vector<TensorDim>& dims) const {
  std::vector<size_t> sizes;
  auto expr = std::dynamic_pointer_cast<ast::ParamExpr>(impl_->expr);
  if (expr) {
    // this handles user inputs
    for (const auto& dim : expr->shape.dims) {
      sizes.emplace_back(dim.size);
    }
  } else if (impl_->dims.size()) {
    // this handles intermediate temporaries (results of previous outputs)
    for (const auto& dim : impl_->dims) {
      if (!dim.impl_->size) {
        throw std::runtime_error("Undefined dimension.");
      }
      sizes.emplace_back(*dim.impl_->size);
    }
  } else {
    // this is the fallback which handles any other case
    auto this_shape = shape();
    for (const auto& dim : this_shape.dims) {
      sizes.emplace_back(dim.size);
    }
  }

  if (dims.size() != sizes.size()) {
    throw std::runtime_error(str(boost::format("TensorShape mismatch in bind_dims(). Tensor shape: %1%, dims: %2%") %
                                 sizes.size() % dims.size()));
  }

  for (size_t i = 0; i < dims.size(); i++) {
    auto& dim = dims[i];
    if (!dim.impl_->size) {
      dim.impl_->size = sizes[i];
    } else if (sizes[i] != *dim.impl_->size) {
      throw std::runtime_error(str(boost::format("bind_dims() mismatch on dim %1%. Required: %2%, Actual: %3%") % i %
                                   *dim.impl_->size % sizes[i]));
    }
  }
}

IndexedTensor Tensor::operator()(const std::vector<TensorIndex>& idxs) const {
  std::vector<size_t> sizes;
  if (impl_->expr) {
    // this handles the case where we're constructing the rhs (e.g. a tensor input)
    auto this_shape = shape();
    if (idxs.size() != this_shape.dims.size()) {
      throw std::runtime_error(
          str(boost::format("Unexpected number of dimensions in contraction. Expected: %1%, Actual: %2%") %
              this_shape.dims.size() % idxs.size()));
    }
  } else {
    // this handles the case where we're constructing the lhs (e.g. a tensor output)
    for (const auto& dim : impl_->dims) {
      if (!dim.impl_->size) {
        throw std::runtime_error("Undefined dimension.");
      }
      sizes.emplace_back(*dim.impl_->size);
    }
  }
  std::vector<std::shared_ptr<ast::PolyExpr>> idx_exprs;
  for (const auto& idx : idxs) {
    idx_exprs.push_back(idx.impl_->expr);
  }
  auto impl = std::make_unique<IndexedTensor::Impl>();
  impl->src = impl_.get();
  impl->expr = std::make_shared<ast::TensorSpecExpr>(impl_->expr, idx_exprs, sizes);
  return IndexedTensor{std::move(impl)};
}

size_t Tensor::dims(const size_t dim) const {
  auto this_shape = shape();
  if (this_shape.dims.size() <= dim) {
    throw std::runtime_error("Requested dimension number higher than number of tensor dimensions");
  }
  return this_shape.dims[dim].size;
}

Tensor Tensor::operator-() const { return Call("neg", {*this}); }
Tensor Tensor::operator~() const { return Call("bit_not", {*this}); }

#define TILE_CC_DEFINE_TENSOR_BINARY_OPS(_op_, _fn_)                                            \
  Tensor operator _op_(const Tensor& lhs, const Tensor& rhs) { return Call(_fn_, lhs, rhs); }   \
  Tensor operator _op_(const Tensor& lhs, int rhs) { return Call(_fn_, lhs, Tensor{rhs}); }     \
  Tensor operator _op_(const Tensor& lhs, int64_t rhs) { return Call(_fn_, lhs, Tensor{rhs}); } \
  Tensor operator _op_(const Tensor& lhs, double rhs) { return Call(_fn_, lhs, Tensor{rhs}); }  \
  Tensor operator _op_(int lhs, const Tensor& rhs) { return Call(_fn_, Tensor{lhs}, rhs); }     \
  Tensor operator _op_(int64_t lhs, const Tensor& rhs) { return Call(_fn_, Tensor{lhs}, rhs); } \
  Tensor operator _op_(double lhs, const Tensor& rhs) { return Call(_fn_, Tensor{lhs}, rhs); }

TILE_CC_DEFINE_TENSOR_BINARY_OPS(+, "add");
TILE_CC_DEFINE_TENSOR_BINARY_OPS(-, "sub");
TILE_CC_DEFINE_TENSOR_BINARY_OPS(*, "mul");
TILE_CC_DEFINE_TENSOR_BINARY_OPS(/, "div");
TILE_CC_DEFINE_TENSOR_BINARY_OPS(==, "cmp_eq");
TILE_CC_DEFINE_TENSOR_BINARY_OPS(!=, "cmp_ne");
TILE_CC_DEFINE_TENSOR_BINARY_OPS(<, "cmp_lt");
TILE_CC_DEFINE_TENSOR_BINARY_OPS(>, "cmp_gt");
TILE_CC_DEFINE_TENSOR_BINARY_OPS(<=, "cmp_le");
TILE_CC_DEFINE_TENSOR_BINARY_OPS(>=, "cmp_ge");
TILE_CC_DEFINE_TENSOR_BINARY_OPS(<<, "bit_left");
TILE_CC_DEFINE_TENSOR_BINARY_OPS(>>, "bit_right");
TILE_CC_DEFINE_TENSOR_BINARY_OPS(&, "bit_and");
TILE_CC_DEFINE_TENSOR_BINARY_OPS(|, "bit_or");
TILE_CC_DEFINE_TENSOR_BINARY_OPS(^, "bit_xor");

Tensor& Tensor::no_defract() {
  auto cion_expr = std::dynamic_pointer_cast<ast::ContractionExpr>(impl_->expr);
  if (!cion_expr) {
    throw std::runtime_error("no_defract can only be specified on a contraction");
  }
  cion_expr->no_defract = true;
  return *this;
}

Tensor& Tensor::use_default(const Tensor& rhs) {
  auto cion_expr = std::dynamic_pointer_cast<ast::ContractionExpr>(impl_->expr);
  if (!cion_expr) {
    throw std::runtime_error("use_default can only be specified on a contraction");
  }
  cion_expr->use_default = rhs.impl_->expr;
  return *this;
}

tile::TensorShape Tensor::shape() const { return EvaluateShape(impl_->expr); }

IndexedTensor::~IndexedTensor() = default;

IndexedTensor::IndexedTensor(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}

IndexedTensor::IndexedTensor(IndexedTensor&& rhs) noexcept : impl_(std::move(rhs.impl_)) {}

void IndexedTensor::Impl::MakeContraction(ast::AggregationOp agg_op, const IndexedTensor& rhs) {
  auto output_spec = std::dynamic_pointer_cast<ast::TensorSpecExpr>(expr);
  if (!output_spec) {
    throw std::runtime_error("oops: out_spec");
  }

  auto cion_expr = std::make_shared<ast::ContractionExpr>();
  cion_expr->agg_op = agg_op;
  cion_expr->output = output_spec;

  auto input_spec = std::dynamic_pointer_cast<ast::TensorSpecExpr>(rhs.impl_->expr);
  if (input_spec) {
    cion_expr->inputs = {input_spec};
  } else {
    auto call_expr = std::dynamic_pointer_cast<ast::CallExpr>(rhs.impl_->expr);
    if (!call_expr) {
      throw std::runtime_error("oops: call_expr");
    }
    if (call_expr->fn == "add") {
      cion_expr->combo_op = ast::CombinationOp::PLUS;
    } else if (call_expr->fn == "mul") {
      cion_expr->combo_op = ast::CombinationOp::MULTIPLY;
    } else if (call_expr->fn == "eq") {
      cion_expr->combo_op = ast::CombinationOp::EQ;
    } else if (call_expr->fn == "cond") {
      cion_expr->combo_op = ast::CombinationOp::COND;
    }
    for (const auto& arg : call_expr->args) {
      auto spec = std::dynamic_pointer_cast<ast::TensorSpecExpr>(arg);
      cion_expr->inputs.push_back(spec);
    }
  }

  ast::ConstraintCollector cc;
  for (const auto& idx : output_spec->index_spec) {
    idx->Accept(&cc);
  }

  for (const auto& tensor : cion_expr->inputs) {
    for (const auto& idx : tensor->index_spec) {
      idx->Accept(&cc);
    }
  }
  cion_expr->constraints = cc.constraints;

  // If the lhs has been optionally named, use it
  cion_expr->name = src->name;
  auto param = std::dynamic_pointer_cast<ast::ParamExpr>(src->expr);
  if (param) {
    cion_expr->name = param->name;
  }
  src->expr = cion_expr;
}

IndexedTensor& IndexedTensor::operator+=(const IndexedTensor& rhs) {
  impl_->MakeContraction(ast::AggregationOp::SUM, rhs);
  return *this;
}

IndexedTensor& IndexedTensor::operator*=(const IndexedTensor& rhs) {
  impl_->MakeContraction(ast::AggregationOp::PROD, rhs);
  return *this;
}

IndexedTensor& IndexedTensor::operator>=(const IndexedTensor& rhs) {
  impl_->MakeContraction(ast::AggregationOp::MAX, rhs);
  return *this;
}

IndexedTensor& IndexedTensor::operator<=(const IndexedTensor& rhs) {
  impl_->MakeContraction(ast::AggregationOp::MIN, rhs);
  return *this;
}

IndexedTensor& IndexedTensor::operator=(const IndexedTensor& rhs) {
  impl_->MakeContraction(ast::AggregationOp::ASSIGN, rhs);
  return *this;
}

IndexedTensor IndexedTensor::Impl::MakeCall(const std::string& fn, const IndexedTensor& rhs) {
  auto impl = std::make_unique<Impl>();
  std::vector<std::shared_ptr<ast::Expr>> args = {expr, rhs.impl_->expr};
  impl->expr = std::make_shared<ast::CallExpr>(fn, args);
  return IndexedTensor{std::move(impl)};
}

IndexedTensor IndexedTensor::operator+(const IndexedTensor& rhs) const { return impl_->MakeCall("add", rhs); }
IndexedTensor IndexedTensor::operator*(const IndexedTensor& rhs) const { return impl_->MakeCall("mul", rhs); }
IndexedTensor IndexedTensor::operator==(const IndexedTensor& rhs) const { return impl_->MakeCall("eq", rhs); }

IndexedTensor cond(const IndexedTensor& lhs, const IndexedTensor& rhs, const IndexedTensor& true_case) {
  return TensorFriend::cond(lhs, rhs, true_case);
}

Tensor Call(const std::string& fn, const std::vector<Tensor>& args) { return TensorFriend::Call(fn, args); }

ast::RunInfo Evaluate(const std::string& name, const std::vector<Tensor>& vars) {
  std::vector<std::shared_ptr<ast::Expr>> exprs;
  for (const auto& var : vars) {
    exprs.push_back(TensorFriend::GetImpl(var)->expr);
  }
  return Evaluate(name, exprs);
}

}  // namespace edsl
}  // namespace plaidml
}  // namespace vertexai