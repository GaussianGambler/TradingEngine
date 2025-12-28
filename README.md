# High-Performance Order Matching Engine

A low-latency, high-throughput order matching engine written in C++ achieving **5-44 million transactions per second** with support for limit orders, market orders, stop orders, and stop-limit orders.

![C++](https://img.shields.io/badge/C%2B%2B-23-blue.svg)
![License](https://img.shields.io/badge/license-MIT-green.svg)
![Platform](https://img.shields.io/badge/platform-Linux%20%7C%20macOS-lightgrey.svg)

## Overview

This matching engine implements a complete order book with price-time priority matching, optimized for minimal latency and maximum throughput. Built for educational and research purposes, it demonstrates production-grade techniques used in financial exchanges and high-frequency trading systems.

### Key Features

- **Ultra-low latency**: ~180ns per order modification, ~730ns per mixed operation
- **High throughput**: 5.4M TPS (complex matching) to 44M TPS (modifications)
- **Complete order types**: Market, Limit, Stop, and Stop-Limit orders
- **Lock-free design**: Lock-free ring buffer for trade reporting
- **Custom memory management**: Arena allocator eliminates heap allocation overhead
- **AVL tree order book**: O(log n) operations with strict balance guarantees
- **Stop order cascades**: Handles triggered stop orders without recursion issues

## Performance Benchmarks

Tested on MacBook Pro with Apple Silicon using Clang++ with -O3 optimization:

| Test Scenario | Throughput | Description |
|--------------|-----------|-------------|
| Statistical Orders | 5.45M TPS | Complex matching with all order types |
| Order Modifications | 44.22M TPS | Pure modification operations |
| Mixed Workload | 6.45M TPS | 75% orders, 15% cancels, 10% modifies |

**Total trades executed**: 1,511,505 across 3M operations

## Architecture

### Core Components

**Order Book**: Dual AVL tree structure maintaining separate buy/sell sides with additional trees for stop orders

**Memory Manager**: Pre-allocated arena with free-lists for Orders and Limits, eliminating runtime allocations

**Ring Buffer**: Lock-free SPSC queue (65,536 entries) for asynchronous trade reporting 

**Order Generator**: Statistical order generation with configurable price distributions for realistic testing

### Design Patterns

- **Price-time priority matching**: FIFO execution at each price level
- **Separate stop order books**: Independent tracking prevents matching interference
- **Batch stop processing**: Aggregates triggered stops before execution
- **Single-threaded matching**: Avoids synchronization overhead in critical path

## Getting Started

### Prerequisites

- C++23 compatible compiler (GCC 11+, Clang 14+, MSVC 2022+)
- POSIX threads library
- CMake 3.15+ (optional, for build management)

### Building

#### Quick Build

clang++ -std=c++23 -O3 -Wall -Wextra -pthread engine.cpp -o engine
./engine

text

#### With CMake

mkdir build && cd build
cmake -DCMAKE_BUILD_TYPE=Release ..
make
./matching_engine

text

### Usage Example

#include "matching_engine.h"

// Initialize components
MemoryManager mm(1000000);
RingBuffer<65536> tradeBuffer;
OrderBook engine(mm, tradeBuffer);

// Place a limit buy order
engine.processOrder(
1001, // order ID
Side::Buy, // side
OrderType::Limit, // type
100, // quantity
29500, // price (cents)
0 // stop price (not used)
);

// Place a market sell order
engine.processOrder(
1002, Side::Sell, OrderType::Market, 50, 0, 0
);

// Modify an existing order
engine.modifyOrder(1001, 150, 29550);

// Cancel an order
engine.cancelOrder(1001);

text

## Code Structure

.
├── engine.cpp # Main implementation
├── README.md # This file
├── docs/
│ ├── architecture.md # Detailed architecture documentation
│ ├── benchmarks.md # Extended benchmark results
│ └── api.md # API reference
└── tests/
└── unit_tests.cpp # Unit tests (TODO)

text

## Benchmark Details

### Test 1: Statistical Orders
Pre-seeds order book with 10,000 limit orders, then processes 1M orders with realistic distributions: 50% limit, 30% market, 10% stop, 10% stop-limit.

### Test 2: Order Modifications
Focuses on modify/cancel operations common in market-making strategies.

### Test 3: Mixed Workload
Simulates production-like behavior with order placement, cancellations, and modifications.

## Implementation Notes

### Stop Order Handling

Stop orders are maintained in separate AVL trees and checked only once per matched order (not per trade) to avoid cascading performance degradation.

### Memory Efficiency

Pre-allocation eliminates heap fragmentation and provides predictable latency. Pool sizes are configurable based on expected order volume .

### Thread Safety

The matching engine is single-threaded (lock-free within matching logic). Trade reporting uses lock-free ring buffer for asynchronous consumption .

## Limitations & Future Work

- Single-threaded matching (parallelization requires careful design)
- No persistence layer (in-memory only)
- No network interface (local benchmarking only)
- Basic order validation (no credit checks, position limits)

**Planned enhancements**:
- [ ] Unit test suite
- [ ] FIX protocol adapter
- [ ] Latency histogram metrics
- [ ] Order book snapshots
- [ ] Alternative data structures (B+ trees, skip lists)

## Contributing

This is an educational/research project. Contributions welcome!

1. Fork the repository
2. Create a feature branch (`git checkout -b feature/enhancement`)
3. Commit changes (`git commit -am 'Add feature'`)
4. Push to branch (`git push origin feature/enhancement`)
5. Open a Pull Request

## References

- [LMAX Disruptor Pattern](https://lmax-exchange.github.io/disruptor/)
- [Matching Engine Design Patterns](https://blog.valensas.com/matching-engine-design)
- [Lock-Free Programming](https://preshing.com/20120612/an-introduction-to-lock-free-programming/)

## License

MIT License - see LICENSE file for details





---

⚠️ **Disclaimer**: This is educational software. Not intended for production trading systems. Use at your own risk.
