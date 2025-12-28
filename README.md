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

- **Order Book**: Dual AVL tree structure maintaining separate buy/sell sides with additional trees for stop orders
- **Memory Manager**: Pre-allocated arena with free-lists for Orders and Limits, eliminating runtime allocations
- **Ring Buffer**: Lock-free SPSC queue (65,536 entries) for asynchronous trade reporting
- **Order Generator**: Statistical order generation with configurable price distributions for realistic testing

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

```
clang++ -std=c++23 -O3 -Wall -Wextra -pthread engine.cpp -o engine
./engine
```
 

#### With GCC

```
g++ -std=c++23 -O3 -Wall -Wextra -pthread engine.cpp -o engine
./engine
```
 

### Usage Example

```
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
engine.processOrder(1002, Side::Sell, OrderType::Market, 50, 0, 0);

// Modify an existing order
engine.modifyOrder(1001, 150, 29550);

// Cancel an order
engine.cancelOrder(1001);
```
 

## Code Structure
```
.
├── engine.cpp # Main implementation
├── README.md # This file
└── LICENSE # MIT License
```
 

## Benchmark Details

### Test 1: Statistical Orders

Pre-seeds order book with 10,000 limit orders, then processes 1M orders with realistic distributions:
- 50% limit orders
- 30% market orders
- 10% stop orders
- 10% stop-limit orders

**Result**: 5.45 Million TPS

### Test 2: Order Modifications

Focuses on modify/cancel operations common in market-making strategies. Tests the performance of order book updates without matching.

**Result**: 44.22 Million TPS

### Test 3: Mixed Workload

Simulates production-like behavior:
- 75% new order placements
- 15% cancellations
- 10% modifications

**Result**: 6.45 Million TPS

## Implementation Notes

### Stop Order Handling

Stop orders are maintained in separate AVL trees and checked only once per matched order (not per individual trade) to avoid cascading performance degradation.

### Memory Efficiency

Pre-allocation eliminates heap fragmentation and provides predictable latency. Pool sizes are configurable based on expected order volume:
- Orders pool: 3x test size
- Limits pool: 0.6x test size

### Thread Safety

The matching engine is single-threaded (lock-free within matching logic). Trade reporting uses a lock-free ring buffer for asynchronous consumption by a separate consumer thread.

## Limitations & Future Work

Current limitations:
- Single-threaded matching (parallelization requires careful design)
- No persistence layer (in-memory only)
- No network interface (local benchmarking only)
- Basic order validation (no credit checks, position limits)
- No market data dissemination


## Technical Details

### Order Types

**Market Order**: Executes immediately at best available price
- Buy: price = INT64_MAX
- Sell: price = 0

**Limit Order**: Executes only at specified price or better
- Rests in order book if not immediately matched

**Stop Order**: Converts to market order when stop price is triggered
- Buy Stop: triggers when price >= stop price
- Sell Stop: triggers when price <= stop price

**Stop-Limit Order**: Converts to limit order when stop price is triggered
- Provides price protection after trigger

### AVL Tree Operations

The order book uses AVL trees for both sides (buy/sell) to maintain sorted price levels:
- Insert: O(log n)
- Delete: O(log n)
- Find min/max: O(log n)
- Height balanced: |left_height - right_height| <= 1

## Contributing

This is an educational/research project. Contributions welcome!

1. Fork the repository
2. Create a feature branch (`git checkout -b feature/enhancement`)
3. Commit changes (`git commit -am 'Add feature'`)
4. Push to branch (`git push origin feature/enhancement`)
5. Open a Pull Request

Please ensure code follows existing style and includes appropriate comments.









---

⚠️ **Disclaimer**: This is educational software for research and learning purposes. Not intended for production trading systems. No warranty provided. Use at your own risk.

**Star this repo if you find it useful!** ⭐
