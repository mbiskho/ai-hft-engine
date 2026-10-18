.DEFAULT_GOAL := help
BUILD ?= build

build: ## Configure and build engine, tests, bench (Release, -march=native)
	cmake -S engine -B $(BUILD) -DCMAKE_BUILD_TYPE=Release && cmake --build $(BUILD) -j

build-onnx: ## Same, linked against ONNX Runtime (onnx: models)
	cmake -S engine -B $(BUILD) -DCMAKE_BUILD_TYPE=Release -DHFT_WITH_ONNX=ON && cmake --build $(BUILD) -j

test: build ## C++ unit tests + Go tests (-race) + Python parity tests
	./$(BUILD)/hft-tests
	cd gateway && go vet ./... && go test -race ./...
	cd research && pip install -e ".[dev]" -q && pytest -q

tsan: ## C++ tests under ThreadSanitizer (ring + engine)
	cmake -S engine -B build-tsan -DCMAKE_BUILD_TYPE=RelWithDebInfo -DHFT_NATIVE=OFF -DCMAKE_CXX_FLAGS="-fsanitize=thread" && \
	  cmake --build build-tsan -j --target hft-tests && ./build-tsan/hft-tests

data: ## 2M synthetic events with a planted imbalance signal
	mkdir -p data && cd research && python -m hft_research.simulate --events 2000000 --out ../data/synthetic.bin

train: ## MLP -> models/signal.onnx, logistic fallback -> models/linear.txt
	cd research && python -m hft_research.train --data ../data/synthetic.bin --out ../models

quantize: ## INT8 model + drift/latency check -> models/quantization.json
	cd research && python -m hft_research.quantize --model ../models/signal.onnx --data ../data/synthetic.bin

backtest: build ## Replay synthetic data through the full pipeline -> reports/backtest.json
	mkdir -p reports
	./$(BUILD)/hft-engine --input data/synthetic.bin --model linear:models/linear.txt --config config/engine.conf \
	  --fills reports/fills.csv --decisions reports/decisions.csv | tee reports/backtest.json
	cd research && python -m hft_research.report --stats ../reports/backtest.json --fills ../reports/fills.csv --out ../reports

bench: build ## Hot-path microbenchmark (pin + quiet the machine first)
	./$(BUILD)/hft-bench 5000000 | tee reports/bench.json

live: ## Paper trade on Binance public data (gateway + engine + QuestDB + Redis)
	docker compose up --build

kill: ## Trip the kill switch on a live run
	redis-cli -p 6382 PUBLISH hft:control kill

images: ## Re-render report mock-ups with headless Chrome
	./scripts/render-mocks.sh

help:
	@grep -hE '^[a-zA-Z_-]+:.*?## ' $(MAKEFILE_LIST) | awk 'BEGIN {FS = ":.*?## "}; {printf "  \033[36m%-11s\033[0m %s\n", $$1, $$2}'

.PHONY: build build-onnx test tsan data train quantize backtest bench live kill images help
