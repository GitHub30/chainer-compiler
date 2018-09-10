#include <algorithm>

#include <chainerx/axes.h>
#include <chainerx/context.h>
#include <chainerx/native/native_backend.h>
#include <chainerx/routines/connection.h>
#include <chainerx/routines/creation.h>
#include <chainerx/routines/indexing.h>
#include <chainerx/routines/linalg.h>
#include <chainerx/routines/logic.h>
#include <chainerx/routines/manipulation.h>
#include <chainerx/routines/math.h>
#include <chainerx/routines/sorting.h>
#include <chainerx/routines/statistics.h>
#include <chainerx/shape.h>

#include <common/log.h>
#include <runtime/gen_xcvm_ops.h>
#include <runtime/xcvm_state.h>

namespace oniku {
namespace runtime {

namespace {

chainerx::OptionalAxes GetXchainerAxes(chainerx::StackVector<int64_t, chainerx::kMaxNdim> axes) {
    if (axes.empty()) return nonstd::nullopt;
    chainerx::Axes xc_axes{axes.begin(), axes.end()};
    return xc_axes;
}

chainerx::Array Sigmoid(chainerx::Array a) {
    // TODO(hamaji): Revisit implementation of this function.
    CHECK(a.dtype() == chainerx::Dtype::kFloat32);
    float f = 1.0f;
    chainerx::Array one = MakeArray(a.dtype(), {}, &f);
    return one / (one + chainerx::Exp(-a));
}

chainerx::Array Tanh(chainerx::Array a) {
    chainerx::Array p = chainerx::Exp(a);
    chainerx::Array m = chainerx::Exp(-a);
    return (p - m) / (p + m);
}

chainerx::Array Pow(chainerx::Array a, chainerx::Array b) {
    return chainerx::Exp(chainerx::Log(a) * b);
}

chainerx::Array ElementwiseMax(chainerx::Array a, chainerx::Array b) {
    // TODO(hamaji): Implement this in ChainerX.
    CHECK_EQ(a.dtype(), b.dtype());
    int64_t an = a.GetTotalSize();
    int64_t bn = b.GetTotalSize();
    chainerx::Array result;
    if (an == 1) {
        result = chainerx::Maximum(chainerx::AsScalar(a), b);
    } else if (bn == 1) {
        result = chainerx::Maximum(a, chainerx::AsScalar(b));
    } else {
        CHECK_EQ(an, bn) << "Max with broadcast not supported yet";
        WARN_ONCE("Slow element-wise Max");
        // Flatten views.
        chainerx::Array av = chainerx::Reshape(a, {an});
        chainerx::Array bv = chainerx::Reshape(b, {an});
        std::vector<chainerx::Array> maxes;
        for (int i = 0; i < an; ++i) {
            chainerx::Array m = chainerx::Maximum(chainerx::AsScalar(av.At({i})), bv.At({i}));
            maxes.push_back(chainerx::Reshape(m, {1}));
        }
        result = Concat(maxes, 0);
        result = chainerx::Reshape(result, a.shape());
    }
    return result;
}

}  // namespace

chainerx::Array InOp::RunImpl(XCVMState* st) {
    return st->Input(name);
}

void OutOp::RunImpl(XCVMState* st, const chainerx::Array& v) {
    st->Output(name, v);
}

void FreeOp::RunImpl(XCVMState* st) {
    st->FreeVar(v);
}

chainerx::Array AddOp::RunImpl(XCVMState* st, const chainerx::Array& a, const chainerx::Array& b) {
    return a + b;
}

chainerx::Array SubOp::RunImpl(XCVMState* st, const chainerx::Array& a, const chainerx::Array& b) {
    return a - b;
}

chainerx::Array MulOp::RunImpl(XCVMState* st, const chainerx::Array& a, const chainerx::Array& b) {
    return a * b;
}

chainerx::Array DivOp::RunImpl(XCVMState* st, const chainerx::Array& a, const chainerx::Array& b) {
    // TODO(hamaji): Come up with a better idea to handle cross device ops.
    if (&a.device() != &b.device() && b.GetTotalSize() == 1) {
        return a / chainerx::AsScalar(b);
    }
    return a / b;
}

chainerx::Array PowOp::RunImpl(XCVMState* st, const chainerx::Array& a, const chainerx::Array& b) {
    return Pow(a, b);
}

chainerx::Array NegOp::RunImpl(XCVMState* st, const chainerx::Array& a) {
    return -a;
}

chainerx::Array ReciprocalOp::RunImpl(XCVMState* st, const chainerx::Array& a) {
    return chainerx::Reciprocal(a);
}

chainerx::Array ExpOp::RunImpl(XCVMState* st, const chainerx::Array& a) {
    return chainerx::Exp(a);
}

chainerx::Array LogOp::RunImpl(XCVMState* st, const chainerx::Array& a) {
    return chainerx::Log(a);
}

chainerx::Array SqrtOp::RunImpl(XCVMState* st, const chainerx::Array& a) {
    return chainerx::Sqrt(a);
}

chainerx::Array TanhOp::RunImpl(XCVMState* st, const chainerx::Array& a) {
    return Tanh(a);
}

chainerx::Array SigmoidOp::RunImpl(XCVMState* st, const chainerx::Array& a) {
    return Sigmoid(a);
}

chainerx::Array ClipOp::RunImpl(XCVMState* st, const chainerx::Array& x) {
    return -chainerx::Maximum(-chainerx::Maximum(x, min), -max);
}

chainerx::Array ArgMaxOp::RunImpl(XCVMState* st, const chainerx::Array& x) {
    chainerx::Array r = chainerx::ArgMax(x, axis);
    if (keepdims) {
        chainerx::Shape shape = x.shape();
        shape[axis] = 1;
        r = chainerx::Reshape(r, shape);
    }
    return r;
}

chainerx::Array MaxOp::RunImpl(XCVMState* st, const std::vector<chainerx::Array>& inputs) {
    CHECK_LT(0, inputs.size());
    chainerx::Array result = inputs[0];
    for (size_t i = 1; i < inputs.size(); ++i) {
        result = ElementwiseMax(result, inputs[i]);
    }
    return result;
}

chainerx::Array HardmaxOp::RunImpl(XCVMState* st, const chainerx::Array& x) {
    chainerx::Shape shape = {1, 1};
    for (size_t i = 0; i < x.shape().size(); ++i) {
        shape[i >= axis] *= x.shape()[i];
    }
    chainerx::Array r = chainerx::ArgMax(chainerx::Reshape(x, shape), 1);
    chainerx::Array e = chainerx::Eye(shape[1], nonstd::nullopt, nonstd::nullopt, x.dtype());
    return chainerx::Reshape(chainerx::Take(e, r, 0), x.shape());
}

chainerx::Array ReduceMaxOp::RunImpl(XCVMState* st, const chainerx::Array& a) {
    return chainerx::AMax(a, GetXchainerAxes(axes), keepdims != 0);
}

chainerx::Array ReduceSumOp::RunImpl(XCVMState* st, const chainerx::Array& a) {
    return chainerx::Sum(a, GetXchainerAxes(axes), keepdims != 0);
}

chainerx::Array ReduceSumSquareOp::RunImpl(XCVMState* st, const chainerx::Array& a) {
    return chainerx::Sum(a * a, GetXchainerAxes(axes), keepdims != 0);
}

chainerx::Array ReduceSumToOp::RunImpl(XCVMState* st, const chainerx::Array& data, const chainerx::Array& shape) {
    const chainerx::Shape& from = data.shape();
    const chainerx::Shape& to = ArrayToShape(shape);
    CHECK_GE(from.size(), to.size()) << "Reduce requested but shape actually expands: " << from << " to=" << to;
    for (int i = 0; i < to.size(); ++i) {
        CHECK_EQ(from[from.size() - i - 1], to[to.size() - i - 1]) << "ReduceSumTo shape mismatches: from=" << from << " to=" << to;
    }
    if (from.size() == to.size()) return data;
    chainerx::Axes axes;
    for (int i = 0; i < from.size() - to.size(); ++i) axes.push_back(i);
    return chainerx::Sum(data, axes, false /* keepdims */);
}

chainerx::Array ReduceMeanOp::RunImpl(XCVMState* st, const chainerx::Array& a) {
    return chainerx::Mean(a, GetXchainerAxes(axes), keepdims != 0);
}

chainerx::Array ConvOp::RunImpl(
        XCVMState* st, const chainerx::Array& x, const chainerx::Array& w, const nonstd::optional<chainerx::Array>& b) {
    return chainerx::Conv(x, w, b, strides, pads);
}

chainerx::Array ConvTransposeOp::RunImpl(
        XCVMState* st, const chainerx::Array& x, const chainerx::Array& w, const nonstd::optional<chainerx::Array>& b) {
    nonstd::optional<chainerx::StackVector<int64_t, chainerx::kMaxNdim>> out_size = nonstd::nullopt;
    if (!output_shape.empty()) {
        // TODO(hamaji): Revisit after getting answer to https://github.com/onnx/onnx/pull/1158
        if (x.ndim() == output_shape.size()) {
            CHECK_LE(2UL, output_shape.size());
            out_size = chainerx::StackVector<int64_t, chainerx::kMaxNdim>(output_shape.begin() + 2, output_shape.end());
        } else {
            out_size = output_shape;
        }
    }
    return chainerx::ConvTranspose(x, w, b, strides, pads, out_size);
}

chainerx::Array ConvTransposeWithDynamicShapeOp::RunImpl(
        XCVMState* st, const chainerx::Array& x, const chainerx::Array& w, const chainerx::Array& output_shape) {
    chainerx::Shape shape = ArrayToShape(output_shape);
    chainerx::StackVector<int64_t, chainerx::kMaxNdim> out_size(shape.begin() + 2, shape.end());
    return chainerx::ConvTranspose(x, w, nonstd::nullopt, strides, pads, out_size);
}

chainerx::Array ConvGradWeightOp::RunImpl(XCVMState* st, const chainerx::Array& w, const chainerx::Array& x, const chainerx::Array& gy) {
    return x.device().ConvGradWeight(w.dtype(), w.shape(), x, gy, strides, pads, false /* cover_all */);
}

chainerx::Array IdentityOp::RunImpl(XCVMState* st, const chainerx::Array& x) {
    return x;
}

chainerx::Array ReluOp::RunImpl(XCVMState* st, const chainerx::Array& x) {
    return chainerx::Maximum(x, 0);
}

chainerx::Array ReluGradOp::RunImpl(XCVMState* st, const chainerx::Array& x, const chainerx::Array& gy) {
    chainerx::Array out = chainerx::EmptyLike(x, x.device());
    x.device().IfLessElseASSA(x, 0, chainerx::Scalar{0, gy.dtype()}, gy, out);
    return out;
}

chainerx::Array FloorOp::RunImpl(XCVMState* st, const chainerx::Array& x) {
    WARN_ONCE("Floor is broken for large floats");
    chainerx::Array out = x.AsType(chainerx::Dtype::kInt64).AsType(x.dtype());
    chainerx::Array negs = (x < chainerx::Zeros({}, x.dtype())).AsType(x.dtype());
    chainerx::Array floats = chainerx::NotEqual(x, out).AsType(x.dtype());
    out -= negs * floats;
    return out;
}

chainerx::Array CeilOp::RunImpl(XCVMState* st, const chainerx::Array& x) {
    WARN_ONCE("Ceil is broken for large values");
    chainerx::Array out = x.AsType(chainerx::Dtype::kInt64).AsType(x.dtype());
    chainerx::Array poses = (x > chainerx::Zeros({}, x.dtype())).AsType(x.dtype());
    chainerx::Array floats = chainerx::NotEqual(x, out).AsType(x.dtype());
    out += poses * floats;
    return out;
}

chainerx::Array ShapeOp::RunImpl(XCVMState* st, const chainerx::Array& data) {
    return ShapeToArray(data.shape());
}

chainerx::Array SizeOp::RunImpl(XCVMState* st, const chainerx::Array& data) {
    int64_t size = data.GetTotalSize();
    return MakeHostArray(chainerx::Dtype::kInt64, {}, &size);
}

chainerx::Array ReshapeOp::RunImpl(XCVMState* st, const chainerx::Array& data, const chainerx::Array& shape) {
    chainerx::Shape s{ArrayToShape(shape)};
    int from_total_size = data.GetTotalSize();
    int to_total_size = 1;
    int to_minus_one_index = -1;
    for (int i = 0; i < s.size(); ++i) {
        int d = s[i];
        CHECK_NE(0, d) << s;
        if (d < 0) {
            to_minus_one_index = i;
        } else {
            to_total_size *= d;
        }
    }
    if (from_total_size != to_total_size) {
        CHECK_GT(from_total_size, to_total_size) << "Reshape from " << data.shape() << " to " << s;
        CHECK_EQ(0, from_total_size % to_total_size) << "Reshape from " << data.shape() << " to " << s;
        CHECK_LE(0, to_minus_one_index) << "Reshape from " << data.shape() << " to " << s;
        s[to_minus_one_index] = from_total_size / to_total_size;
    }
    return chainerx::Reshape(data, s);
}

chainerx::Array ExpandOp::RunImpl(XCVMState* st, const chainerx::Array& data, const chainerx::Array& shape) {
    return chainerx::BroadcastTo(data, ArrayToShape(shape));
}

chainerx::Array SqueezeOp::RunImpl(XCVMState* st, const chainerx::Array& data) {
    chainerx::Shape shape;
    for (size_t i = 0; i < data.shape().size(); ++i) {
        if (std::find(axes.begin(), axes.end(), i) == axes.end()) {
            shape.push_back(data.shape()[i]);
        } else {
            CHECK_EQ(1, data.shape()[i]) << "Cannot squeeze a dimension whose size is not 1: " << data.shape();
        }
    }
    return chainerx::Reshape(data, shape);
}

chainerx::Array UnsqueezeOp::RunImpl(XCVMState* st, const chainerx::Array& data) {
    chainerx::Shape shape = data.shape();
    for (int d : axes) {
        CHECK_LE(d, shape.size()) << "Unsqueezing axis out of bound: " << d;
        shape.insert(shape.begin() + d, 1);
    }
    return chainerx::Reshape(data, shape);
}

chainerx::Array SliceOp::RunImpl(XCVMState* st, const chainerx::Array& data) {
    std::vector<chainerx::ArrayIndex> indices(data.ndim(), chainerx::Slice());
    for (size_t i = 0; i < axes.size(); ++i) {
        int64_t axis = axes[i];
        int64_t start = starts[i];
        int64_t end = ends[i];
        indices[axis] = chainerx::Slice(start, end, 1);
    }
    return data.At(indices);
}

chainerx::Array DynamicSliceOp::RunImpl(
        XCVMState* st,
        const chainerx::Array& data,
        const chainerx::Array& starts,
        const chainerx::Array& ends,
        const nonstd::optional<chainerx::Array>& axes) {
    CHECK_EQ(1, starts.ndim());
    CHECK_EQ(1, ends.ndim());
    std::vector<chainerx::ArrayIndex> indices(data.ndim(), chainerx::Slice());
    for (int64_t i = 0; i < starts.shape()[0]; ++i) {
        int64_t axis = axes.has_value() ? int64_t(chainerx::AsScalar(axes->At({i}))) : i;
        int64_t start = int64_t(chainerx::AsScalar(starts.At({i})));
        int64_t end = int64_t(chainerx::AsScalar(ends.At({i})));
        indices[axis] = chainerx::Slice(start, end, 1);
    }
    return data.At(indices);
}

chainerx::Array GatherOp::RunImpl(XCVMState* st, const chainerx::Array& data, const chainerx::Array& indices) {
    return data.Take(indices, axis);
}

chainerx::Array SelectItemOp::RunImpl(XCVMState* st, const chainerx::Array& data, const chainerx::Array& indices) {
    CHECK_EQ(2UL, data.shape().size()) << "TODO(hamaji): Support SelectItem for non-2D array";
    int64_t batch_size = data.shape()[0];
    int64_t num_classes = data.shape()[1];
    int64_t total_size = batch_size * num_classes;
    chainerx::Array take_indices = indices + chainerx::Arange(0, total_size, num_classes);
    return data.Reshape({total_size}).Take(take_indices, 0);
}

chainerx::Array SelectItemGradOp::RunImpl(
        XCVMState* st, const chainerx::Array& gy, const chainerx::Array& indices, const chainerx::Array& shape_array) {
    chainerx::Shape shape{ArrayToShape(shape_array)};
    CHECK_EQ(2, shape.size()) << "TODO(hamaji): Support SelectItem for non-2D array";
    int64_t batch_size = shape[0];
    int64_t num_classes = shape[1];
    int64_t total_size = batch_size * num_classes;
    chainerx::Array out = chainerx::Zeros({total_size}, gy.dtype());
    chainerx::Array take_indices = indices + chainerx::Arange(0, total_size, num_classes);
    out.device().AddAt(out, take_indices, 0, gy, out);
    return out.Reshape(shape);
}

chainerx::Array ConcatOp::RunImpl(XCVMState* st, const std::vector<chainerx::Array>& inputs) {
    return Concat(inputs, axis);
}

std::vector<chainerx::Array> SplitOp::RunImpl(XCVMState* st, const chainerx::Array& input) {
    std::vector<int64_t> lens{split.begin(), split.end()};
    if (lens.empty()) {
        int64_t dim = input.shape()[axis];
        int num_splits = outputs.size();
        CHECK_EQ(0, dim % num_splits) << dim;
        lens = std::vector<int64_t>(num_splits, dim / num_splits);
    }
    return Split(input, lens, axis);
}

chainerx::Array TransposeOp::RunImpl(XCVMState* st, const chainerx::Array& data) {
    return chainerx::Transpose(data, GetXchainerAxes(perm));
}

chainerx::Array PadOp::RunImpl(XCVMState* st, const chainerx::Array& data) {
    CHECK_EQ(data.ndim() * 2, pads.size());
    chainerx::Shape shape = data.shape();
    std::vector<chainerx::ArrayIndex> indices;
    for (int i = 0; i < shape.size(); ++i) {
        indices.push_back(chainerx::Slice(pads[i], pads[i] + shape[i]));
        shape[i] += pads[i] + pads[i + shape.size()];
    }
    chainerx::Array result = chainerx::Full(shape, value, data.dtype(), data.device());
    result.device().Copy(data, result.At(indices));
    return result;
}

chainerx::Array SoftmaxOp::RunImpl(XCVMState* st, const chainerx::Array& input) {
    return chainerx::Exp(chainerx::LogSoftmax(input, chainerx::OptionalAxes{static_cast<char>(axis)}));
}

chainerx::Array LogSoftmaxOp::RunImpl(XCVMState* st, const chainerx::Array& input) {
    return chainerx::LogSoftmax(input, chainerx::OptionalAxes{static_cast<char>(axis)});
}

chainerx::Array MatMulOp::RunImpl(XCVMState* st, const chainerx::Array& a, const chainerx::Array& b) {
    return chainerx::Dot(a, b);
}

chainerx::Array GemmOp::RunImpl(XCVMState* st, const chainerx::Array& a, const chainerx::Array& b, const chainerx::Array& c) {
    chainerx::Array xa = a;
    chainerx::Array xb = b;
    if (trans_a) xa = chainerx::Transpose(xa);
    if (trans_b) xb = chainerx::Transpose(xb);

    // TODO(hamaji): I don't understand the semantics of
    // "undirectional broadcasting". This implementation handles what
    // chainer does (e.g., (3, 4, 2, 2) @ (16, 2) => (3, 2)).
    // https://github.com/onnx/onnx/blob/master/docs/Broadcasting.md
    if (xa.shape().size() > 2) {
        int last_dim = 1;
        for (size_t i = 1; i < xa.shape().size(); ++i) {
            last_dim *= xa.shape()[i];
        }
        xa = chainerx::Reshape(xa, chainerx::Shape{xa.shape()[0], last_dim});
    }
    if (xb.shape().size() > 2) {
        int last_dim = 1;
        for (size_t i = 1; i < xb.shape().size(); ++i) {
            last_dim *= xb.shape()[i];
        }
        xb = chainerx::Reshape(xb, chainerx::Shape{xb.shape()[0], last_dim});
    }

    chainerx::Array r = chainerx::Dot(xa, xb);
    if (alpha != 1.0) r *= alpha;
    if (beta == 0.0) return r;
    chainerx::Array xc = c;
    if (beta != 1.0) xc = xc * beta;
    return r + xc;
}

std::tuple<chainerx::Array, chainerx::Array, chainerx::Array> LSTMOp::RunImpl(
        XCVMState* st,
        const chainerx::Array& x,
        const chainerx::Array& w,
        const chainerx::Array& r,
        const nonstd::optional<chainerx::Array>& b,
        const nonstd::optional<chainerx::Array>& sequence_lens,
        const nonstd::optional<chainerx::Array>& initial_h,
        const nonstd::optional<chainerx::Array>& initial_c,
        const nonstd::optional<chainerx::Array>& p) {
    // X: [seq_length, batch_size, input_size]
    // W: [num_directions, 4 * hidden_size, input_size]
    // R: [num_directions, 4 * hidden_size, hidden_size]
    // B: [num_directions, 8 * hidden_size]
    // TODO(hamaji): They cannot be tested as ONNX does not have test cases.
    CHECK_EQ(1, w.shape()[0]) << "Multi-directional LSTM is not implemented yet";
    if (sequence_lens.has_value()) {
        WARN_ONCE("LSTM with sequence_lens is not supported yet");
    }

    int64_t seq_length = x.shape()[0];
    int64_t batch_size = x.shape()[1];
    CHECK_EQ(0, w.shape()[1] % 4);
    int64_t hidden_size = w.shape()[1] / 4;
    CHECK_EQ(4 * hidden_size, r.shape()[1]);
    if (b.has_value()) CHECK_EQ(8 * hidden_size, b.value().shape()[1]);

    chainerx::Array wt = chainerx::Transpose(chainerx::Squeeze(w, {0}));
    chainerx::Array rt = chainerx::Transpose(chainerx::Squeeze(r, {0}));
    chainerx::Array h =
            initial_h.has_value() ? chainerx::Squeeze(initial_h.value(), {0}) : chainerx::Zeros({batch_size, hidden_size}, x.dtype());
    chainerx::Array c =
            initial_c.has_value() ? chainerx::Squeeze(initial_c.value(), {0}) : chainerx::Zeros({batch_size, hidden_size}, x.dtype());
    std::vector<chainerx::ArrayIndex> indices(2, chainerx::Slice());
    chainerx::Array bm;
    if (b.has_value()) {
        chainerx::Array bs = chainerx::Squeeze(b.value(), {0});
        chainerx::Array b1 = bs.At({chainerx::Slice(0, 4 * hidden_size)});
        chainerx::Array b2 = bs.At({chainerx::Slice(4 * hidden_size, 8 * hidden_size)});
        bm = b1 + b2;
    }
    chainerx::Array pi, po, pf;
    if (p.has_value()) {
        chainerx::Array ps = chainerx::Squeeze(p.value(), {0});
        pi = ps.At({chainerx::Slice(0, hidden_size)});
        po = ps.At({chainerx::Slice(hidden_size, 2 * hidden_size)});
        pf = ps.At({chainerx::Slice(2 * hidden_size, 3 * hidden_size)});
    }

    chainerx::Array output = chainerx::Zeros({seq_length, batch_size, hidden_size}, x.dtype());
    for (int64_t time = 0; time < x.shape()[0]; ++time) {
        chainerx::Array cur_x = x.At({time});
        chainerx::Array gates = chainerx::Dot(cur_x, wt) + chainerx::Dot(h, rt);
        if (b.has_value()) {
            gates += bm;
        }
        indices[1] = chainerx::Slice({0, hidden_size});
        chainerx::Array i = gates.At(indices);
        indices[1] = chainerx::Slice({hidden_size, hidden_size * 2});
        chainerx::Array o = gates.At(indices);
        indices[1] = chainerx::Slice({hidden_size * 2, hidden_size * 3});
        chainerx::Array f = gates.At(indices);
        indices[1] = chainerx::Slice({hidden_size * 3, hidden_size * 4});
        chainerx::Array nc = gates.At(indices);

        if (p.has_value()) {
            i += pi * c;
            f += pf * c;
            o += po * c;
        }
        i = Sigmoid(i);
        f = Sigmoid(f);
        nc = Tanh(nc);
        c = f * c + i * nc;
        o = Sigmoid(o);
        h = o * Tanh(c);

        output.At({time}) += h;
    }
    h = chainerx::Reshape(h, {1, h.shape()[0], h.shape()[1]});
    c = chainerx::Reshape(c, {1, c.shape()[0], c.shape()[1]});
    return std::make_tuple(output, h, c);
}

chainerx::Array EqualOp::RunImpl(XCVMState* st, const chainerx::Array& a, const chainerx::Array& b) {
    return chainerx::Equal(a, b);
}

chainerx::Array GreaterOp::RunImpl(XCVMState* st, const chainerx::Array& a, const chainerx::Array& b) {
    return chainerx::Greater(a, b);
}

chainerx::Array GreaterEqualOp::RunImpl(XCVMState* st, const chainerx::Array& a, const chainerx::Array& b) {
    // TODO(hamaji): This is an incorrect implementation for NaN.
    return chainerx::LogicalNot(chainerx::Greater(b, a));
}

chainerx::Array NotOp::RunImpl(XCVMState* st, const chainerx::Array& x) {
    return chainerx::LogicalNot(x);
}

chainerx::Array CastOp::RunImpl(XCVMState* st, const chainerx::Array& input) {
    return input.AsType(static_cast<chainerx::Dtype>(to));
}

chainerx::Array IntScalarConstantOp::RunImpl(XCVMState* st) {
    chainerx::Device& device = host ? chainerx::GetNativeBackend().GetDevice(0) : chainerx::GetDefaultDevice();
    return chainerx::Full({}, value, static_cast<chainerx::Dtype>(dtype), device);
}

chainerx::Array FloatScalarConstantOp::RunImpl(XCVMState* st) {
    chainerx::Device& device = host ? chainerx::GetNativeBackend().GetDevice(0) : chainerx::GetDefaultDevice();
    return chainerx::Full({}, value, static_cast<chainerx::Dtype>(dtype), device);
}

chainerx::Array IntConstantOp::RunImpl(XCVMState* st) {
    auto make = host ? MakeHostArray : MakeArray;
    chainerx::Array a = make(chainerx::Dtype::kInt64, chainerx::Shape(shape), value.data());
    return a.AsType(static_cast<chainerx::Dtype>(dtype));
}

chainerx::Array FloatConstantOp::RunImpl(XCVMState* st) {
    auto make = host ? MakeHostArray : MakeArray;
    chainerx::Array a = make(chainerx::Dtype::kFloat64, chainerx::Shape(shape), value.data());
    return a.AsType(static_cast<chainerx::Dtype>(dtype));
}

void JmpTrueOp::RunImpl(XCVMState* st, const chainerx::Array& cond) {
    if (static_cast<bool>(chainerx::AsScalar(cond))) {
        st->set_pc(pc - 1);
    }
}

void JmpFalseOp::RunImpl(XCVMState* st, const chainerx::Array& cond) {
    if (!static_cast<bool>(chainerx::AsScalar(cond))) {
        st->set_pc(pc - 1);
    }
}

}  // namespace runtime
}  // namespace oniku
