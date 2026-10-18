// hft-engine: three threads, two SPSC rings, no locks.
//
//   feed thread    read 64-byte events (file replay or Unix socket from the Go gateway) -> ring A
//   engine thread  ring A -> book, features, model, risk, paper exchange -> ring B
//   writer thread  ring B -> fills.csv / decisions.csv (all I/O off the hot path)
//
//   hft-engine --input data/btcusdt.bin --model linear:models/linear.txt --config config/engine.conf
//   hft-engine --input unix:/tmp/hft-gateway.sock --model onnx:models/signal.int8.onnx --pin 2,3,4

#include <fcntl.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include <atomic>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <functional>
#include <iostream>
#include <map>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#ifdef __linux__
#include <pthread.h>
#include <sched.h>
#endif

#include "hft/engine.hpp"
#include "hft/spsc_ring.hpp"

using namespace hft;

namespace {

struct Args {
  std::string input, model = "linear:models/linear.txt", config, fills = "fills.csv", decisions = "decisions.csv";
  std::vector<int> pin;  // cores for feed, engine, writer
};

Args parse_args(int argc, char** argv) {
  Args a;
  for (int i = 1; i < argc; ++i) {
    std::string k = argv[i];
    auto next = [&]() -> std::string {
      if (i + 1 >= argc) throw std::runtime_error("missing value for " + k);
      return argv[++i];
    };
    if (k == "--input") a.input = next();
    else if (k == "--model") a.model = next();
    else if (k == "--config") a.config = next();
    else if (k == "--fills") a.fills = next();
    else if (k == "--decisions") a.decisions = next();
    else if (k == "--pin") {
      std::stringstream ss(next());
      for (std::string c; std::getline(ss, c, ',');) a.pin.push_back(std::stoi(c));
    } else throw std::runtime_error("unknown argument " + k);
  }
  if (a.input.empty()) throw std::runtime_error("--input is required");
  return a;
}

// key = value, '#' comments. Unknown keys are an error: a typo in a risk
// limit must not silently fall back to the default.
EngineConfig load_config(const std::string& path) {
  EngineConfig c;
  if (path.empty()) return c;
  std::ifstream in(path);
  if (!in) throw std::runtime_error("cannot open config " + path);
  std::map<std::string, std::function<void(const std::string&)>> set = {
      {"strategy.buy_threshold", [&](auto v) { c.strategy.buy_threshold = std::stof(v); }},
      {"strategy.sell_threshold", [&](auto v) { c.strategy.sell_threshold = std::stof(v); }},
      {"strategy.order_qty", [&](auto v) { c.strategy.order_qty = std::stoll(v); }},
      {"strategy.cooldown_ms", [&](auto v) { c.strategy.cooldown_ns = std::stoull(v) * 1'000'000; }},
      {"strategy.aggression_ticks", [&](auto v) { c.strategy.aggression_ticks = std::stoll(v); }},
      {"risk.max_order_qty", [&](auto v) { c.risk.max_order_qty = std::stoll(v); }},
      {"risk.max_position", [&](auto v) { c.risk.max_position = std::stoll(v); }},
      {"risk.max_notional", [&](auto v) { c.risk.max_notional = std::stod(v); }},
      {"risk.price_collar_bps", [&](auto v) { c.risk.price_collar_bps = std::stod(v); }},
      {"risk.max_orders_per_sec", [&](auto v) { c.risk.max_orders_per_sec = static_cast<std::uint32_t>(std::stoul(v)); }},
      {"risk.max_drawdown", [&](auto v) { c.risk.max_drawdown = std::stod(v); }},
      {"exec.latency_us", [&](auto v) { c.exec.latency_ns = std::stoull(v) * 1'000; }},
      {"exec.fee_bps", [&](auto v) { c.exec.fee_bps = std::stod(v); }},
  };
  std::string line;
  for (int n = 1; std::getline(in, line); ++n) {
    if (auto h = line.find('#'); h != std::string::npos) line.erase(h);
    const auto eq = line.find('=');
    if (eq == std::string::npos) continue;
    auto trim = [](std::string s) {
      s.erase(0, s.find_first_not_of(" \t"));
      s.erase(s.find_last_not_of(" \t\r") + 1);
      return s;
    };
    const std::string k = trim(line.substr(0, eq)), v = trim(line.substr(eq + 1));
    auto it = set.find(k);
    if (it == set.end()) throw std::runtime_error(path + ":" + std::to_string(n) + ": unknown key " + k);
    it->second(v);
  }
  return c;
}

int open_input(const std::string& spec) {
  if (spec.rfind("unix:", 0) == 0) {
    const int fd = ::socket(AF_UNIX, SOCK_STREAM, 0);
    sockaddr_un addr{};
    addr.sun_family = AF_UNIX;
    std::strncpy(addr.sun_path, spec.c_str() + 5, sizeof(addr.sun_path) - 1);
    if (fd < 0 || ::connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
      throw std::runtime_error("cannot connect to " + spec);
    }
    return fd;
  }
  const int fd = ::open(spec.c_str(), O_RDONLY);
  if (fd < 0) throw std::runtime_error("cannot open " + spec);
  return fd;
}

void pin_to(int core) {
#ifdef __linux__
  if (core < 0) return;
  cpu_set_t set;
  CPU_ZERO(&set);
  CPU_SET(core, &set);
  pthread_setaffinity_np(pthread_self(), sizeof(set), &set);
#else
  (void)core;  // macOS has no hard affinity; tuning applies on Linux only (docs/linux-tuning.md)
#endif
}

constexpr std::uint32_t kEndOfStream = 0xFFFFFFFFu;

template <typename T>
void push_spin(SpscRing<T>& ring, const T& v) {
  while (!ring.try_push(v)) {
    // Busy-wait: the consumer is a dedicated core and will catch up within
    // microseconds. Yielding to the scheduler costs far more.
  }
}

}  // namespace

int main(int argc, char** argv) try {
  const Args args = parse_args(argc, argv);
  const EngineConfig cfg = load_config(args.config);
  auto model = load_model(args.model);

  SpscRing<MarketEvent> in_ring(1 << 16);
  SpscRing<OutRecord> out_ring(1 << 14);
  Engine engine(cfg, *model);
  std::atomic<std::uint64_t> dropped_out{0};

  const int fd = open_input(args.input);
  const bool replay = args.input.rfind("unix:", 0) != 0;
  const auto core = [&](std::size_t i) { return i < args.pin.size() ? args.pin[i] : -1; };

  const Nanos wall_start = steady_ns();

  // ---- feed ----
  std::thread feed([&] {
    pin_to(core(0));
    std::vector<MarketEvent> buf(4096);
    std::size_t carry = 0;  // bytes of a partial event left over from the last read
    auto* bytes = reinterpret_cast<char*>(buf.data());
    for (;;) {
      const ssize_t n = ::read(fd, bytes + carry, buf.size() * sizeof(MarketEvent) - carry);
      if (n <= 0) break;
      const std::size_t total = carry + static_cast<std::size_t>(n);
      const std::size_t whole = total / sizeof(MarketEvent);
      for (std::size_t i = 0; i < whole; ++i) {
        MarketEvent e = buf[i];
        // Replay: re-stamp with the local clock so signal latency measures
        // this pipeline, not the age of the recording.
        if (replay) e.recv_ts_ns = steady_ns();
        push_spin(in_ring, e);
      }
      carry = total - whole * sizeof(MarketEvent);
      std::memmove(bytes, bytes + whole * sizeof(MarketEvent), carry);
    }
    MarketEvent end{};
    end.type = EventType::Heartbeat;
    end.flags = kEndOfStream;
    push_spin(in_ring, end);
  });

  // ---- writer ----
  std::thread writer([&] {
    pin_to(core(2));
    std::FILE* fills = std::fopen(args.fills.c_str(), "w");
    std::FILE* decisions = std::fopen(args.decisions.c_str(), "w");
    if (!fills || !decisions) {
      std::cerr << "cannot open output files\n";
      std::exit(1);
    }
    std::fprintf(fills, "market_ns,side,qty,price,position_after,pnl\n");
    std::fprintf(decisions, "market_ns,kind,side,p_up,qty,limit,position,reason\n");
    OutRecord r;
    for (;;) {
      if (!out_ring.try_pop(r)) {
        std::this_thread::yield();  // the writer is allowed to be slow
        continue;
      }
      if (r.kind == OutRecord::End) break;
      const char* side = r.side == OrderSide::Buy ? "buy" : "sell";
      if (r.kind == OutRecord::FillRec) {
        std::fprintf(fills, "%llu,%s,%lld,%lld,%lld,%.2f\n", static_cast<unsigned long long>(r.market_ns), side,
                     static_cast<long long>(r.qty), static_cast<long long>(r.price),
                     static_cast<long long>(r.position_after), r.pnl);
      } else {
        const char* kind = r.kind == OutRecord::Decision ? "order" : r.kind == OutRecord::Reject ? "reject" : "kill";
        std::fprintf(decisions, "%llu,%s,%s,%.4f,%lld,%lld,%lld,%s\n", static_cast<unsigned long long>(r.market_ns), kind,
                     side, r.p_up, static_cast<long long>(r.qty), static_cast<long long>(r.price),
                     static_cast<long long>(r.position_after), to_string(static_cast<RiskReason>(r.reason)));
      }
    }
    std::fclose(fills);
    std::fclose(decisions);
  });

  // ---- engine (this thread) ----
  pin_to(core(1));
  // The engine must never block on the writer. If the output ring is full the
  // record is dropped and counted; trading state is unaffected.
  auto emit = [&](const OutRecord& r) {
    if (!out_ring.try_push(r)) dropped_out.fetch_add(1, std::memory_order_relaxed);
  };
  MarketEvent e;
  for (;;) {
    if (!in_ring.try_pop(e)) continue;  // spin: this core belongs to the engine
    if (e.type == EventType::Heartbeat && e.flags == kEndOfStream) break;
    engine.on_event(e, replay ? e.recv_ts_ns : steady_ns(), emit);
  }
  push_spin(out_ring, OutRecord{OutRecord::End, 0, OrderSide::Buy, 0, 0, 0, 0, 0, 0});

  feed.join();
  writer.join();
  ::close(fd);
  const double secs = static_cast<double>(steady_ns() - wall_start) * 1e-9;

  // ---- report (JSON on stdout) ----
  const auto& cl = engine.compute_latency();
  const auto& sl = engine.signal_latency();
  const auto& rk = engine.risk();
  std::printf("{\n");
  std::printf("  \"model\": \"%s\",\n", model->name());
  std::printf("  \"events\": %llu, \"book_updates\": %llu, \"seconds\": %.3f, \"events_per_sec\": %.0f,\n",
              static_cast<unsigned long long>(engine.events()), static_cast<unsigned long long>(engine.book_updates()),
              secs, static_cast<double>(engine.events()) / secs);
  std::printf("  \"decisions\": %llu, \"orders\": %llu, \"fills\": %llu, \"gaps\": %llu, \"crossed_books\": %llu,\n",
              static_cast<unsigned long long>(engine.decisions()), static_cast<unsigned long long>(engine.orders()),
              static_cast<unsigned long long>(engine.fills()), static_cast<unsigned long long>(engine.gaps()),
              static_cast<unsigned long long>(engine.book().crossed_events()));
  std::printf("  \"compute_ns\": {\"p50\": %llu, \"p99\": %llu, \"p999\": %llu, \"max\": %llu},\n",
              (unsigned long long)cl.percentile(50), (unsigned long long)cl.percentile(99),
              (unsigned long long)cl.percentile(99.9), (unsigned long long)cl.max());
  std::printf("  \"signal_ns\": {\"p50\": %llu, \"p99\": %llu, \"p999\": %llu, \"max\": %llu},\n",
              (unsigned long long)sl.percentile(50), (unsigned long long)sl.percentile(99),
              (unsigned long long)sl.percentile(99.9), (unsigned long long)sl.max());
  std::printf("  \"risk_rejections\": {");
  for (int r = 1; r < static_cast<int>(RiskReason::Count); ++r) {
    std::printf("%s\"%s\": %llu", r > 1 ? ", " : "", to_string(static_cast<RiskReason>(r)),
                static_cast<unsigned long long>(rk.count(static_cast<RiskReason>(r))));
  }
  std::printf("},\n");
  std::printf("  \"kill_switch\": %s, \"kill_reason\": \"%s\",\n", rk.killed() ? "true" : "false", to_string(rk.kill_reason()));
  std::printf("  \"position\": %lld, \"max_abs_position\": %llu, \"position_limit\": %lld,\n",
              static_cast<long long>(engine.position()), static_cast<unsigned long long>(engine.max_abs_position()),
              static_cast<long long>(cfg.risk.max_position));
  std::printf("  \"pnl_ticks_lots\": %.2f, \"fees\": %.2f, \"max_drawdown\": %.2f,\n", engine.pnl(), engine.fees(), engine.max_drawdown());
  std::printf("  \"dropped_output_records\": %llu\n}\n", static_cast<unsigned long long>(dropped_out.load()));
  return 0;
} catch (const std::exception& ex) {
  std::cerr << "hft-engine: " << ex.what() << "\n";
  return 1;
}
