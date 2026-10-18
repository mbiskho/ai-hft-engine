#pragma once
// Signal models: features -> probability that mid moves up over the horizon.
//
// LinearModel is the always-available fallback (logistic regression weights
// exported by research/train.py). OnnxModel runs the quantized MLP through
// ONNX Runtime with input and output tensors bound to buffers allocated once,
// so a prediction is a Run() call with no heap traffic. Built only with
// -DHFT_WITH_ONNX=ON.

#include <cmath>
#include <fstream>
#include <memory>
#include <stdexcept>
#include <string>

#include "features.hpp"

#ifdef HFT_WITH_ONNX
#include <onnxruntime_cxx_api.h>
#endif

namespace hft {

class Model {
 public:
  virtual ~Model() = default;
  virtual float predict(const FeatureVec& x) noexcept = 0;
  virtual const char* name() const noexcept = 0;
};

class LinearModel final : public Model {
 public:
  LinearModel(const std::array<float, kNumFeatures>& w, float b) : w_(w), b_(b) {}

  // File: 8 weights then the bias, whitespace separated (research/train.py --export-linear).
  static std::unique_ptr<LinearModel> load(const std::string& path) {
    std::ifstream in(path);
    if (!in) throw std::runtime_error("cannot open linear model: " + path);
    std::array<float, kNumFeatures> w{};
    float b = 0;
    for (auto& v : w) in >> v;
    in >> b;
    if (!in) throw std::runtime_error("linear model needs 9 numbers: " + path);
    return std::make_unique<LinearModel>(w, b);
  }

  float predict(const FeatureVec& x) noexcept override {
    float z = b_;
    for (std::size_t i = 0; i < kNumFeatures; ++i) z += w_[i] * x[i];
    return 1.0f / (1.0f + std::exp(-z));
  }
  const char* name() const noexcept override { return "linear"; }

 private:
  std::array<float, kNumFeatures> w_;
  float b_;
};

#ifdef HFT_WITH_ONNX
class OnnxModel final : public Model {
 public:
  explicit OnnxModel(const std::string& path)
      : env_(ORT_LOGGING_LEVEL_WARNING, "hft"), session_(nullptr), mem_(Ort::MemoryInfo::CreateCpu(OrtArenaAllocator, OrtMemTypeDefault)) {
    Ort::SessionOptions opt;
    opt.SetIntraOpNumThreads(1);  // one core, no thread-pool wakeups on the hot path
    opt.SetInterOpNumThreads(1);
    opt.SetGraphOptimizationLevel(GraphOptimizationLevel::ORT_ENABLE_ALL);
    opt.AddConfigEntry("session.intra_op.allow_spinning", "0");
    session_ = Ort::Session(env_, path.c_str(), opt);
    const int64_t in_shape[2] = {1, static_cast<int64_t>(kNumFeatures)};
    const int64_t out_shape[2] = {1, 1};
    input_ = Ort::Value::CreateTensor<float>(mem_, in_.data(), in_.size(), in_shape, 2);
    output_ = Ort::Value::CreateTensor<float>(mem_, &out_, 1, out_shape, 2);
    // Warm up: first Run() pays for lazy initialisation.
    for (int i = 0; i < 100; ++i) run();
  }

  float predict(const FeatureVec& x) noexcept override {
    in_ = x;  // the input tensor aliases in_
    run();
    return out_;
  }
  const char* name() const noexcept override { return "onnx"; }

 private:
  void run() noexcept {
    static const char* in_names[] = {"features"};
    static const char* out_names[] = {"p_up"};
    session_.Run(run_opts_, in_names, &input_, 1, out_names, &output_, 1);
  }

  Ort::Env env_;
  Ort::Session session_;
  Ort::MemoryInfo mem_;
  Ort::RunOptions run_opts_;
  FeatureVec in_{};
  float out_ = 0.5f;
  Ort::Value input_{nullptr};
  Ort::Value output_{nullptr};
};
#endif

inline std::unique_ptr<Model> load_model(const std::string& spec) {
  // "linear:path" or "onnx:path"
  const auto colon = spec.find(':');
  const std::string kind = spec.substr(0, colon);
  const std::string path = colon == std::string::npos ? "" : spec.substr(colon + 1);
  if (kind == "linear") return LinearModel::load(path);
#ifdef HFT_WITH_ONNX
  if (kind == "onnx") return std::make_unique<OnnxModel>(path);
#endif
  throw std::runtime_error("unknown or unavailable model '" + spec + "' (onnx needs -DHFT_WITH_ONNX=ON)");
}

}  // namespace hft
