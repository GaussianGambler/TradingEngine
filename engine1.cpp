#include <iostream>
#include <vector>
#include <cstdint>
#include <chrono>
#include <algorithm>
#include <unordered_map>
#include <functional>
#include <array>
#include <atomic>
#include <thread>
#include <random>

// --- 1. DATA STRUCTURES ---
enum class Side
{
	Buy,
	Sell
};
enum class OrderType
{
	Market,
	Limit,
	Stop,
	StopLimit
};

struct TradeReport
{
	uint64_t takerId;
	uint64_t makerId;
	uint32_t qty;
	int64_t price;
	uint64_t timestamp;
};

struct Order
{
	uint64_t id;
	Side side;
	OrderType type;
	uint32_t shares;
	int64_t price;
	int64_t stopPrice;
	Order *next = nullptr, *prev = nullptr, *nextFree = nullptr;
	struct Limit *parentLimit = nullptr;
};

struct Limit
{
	int64_t price;
	Order *head = nullptr, *tail = nullptr;
	Limit *left = nullptr, *right = nullptr, *nextFree = nullptr;
	int height = 1;

	Limit() = default;
	explicit Limit(int64_t p) : price(p) {}
};

struct TriggeredStop
{
	uint64_t originalId;
	Side side;
	OrderType convertToType;
	uint32_t shares;
	int64_t limitPrice;
};

// --- 2. LOCK-FREE RING BUFFER ---
template <size_t SIZE>
class RingBuffer
{
	static_assert((SIZE & (SIZE - 1)) == 0, "SIZE must be power of 2");
	std::array<TradeReport, SIZE> buffer;
	alignas(64) std::atomic<uint64_t> writePos{0};
	alignas(64) std::atomic<uint64_t> readPos{0};

public:
	bool push(const TradeReport &t)
	{
		uint64_t wp = writePos.load(std::memory_order_relaxed);
		if (wp - readPos.load(std::memory_order_acquire) >= SIZE)
			return false;
		buffer[wp & (SIZE - 1)] = t;
		writePos.store(wp + 1, std::memory_order_release);
		return true;
	}

	bool pop(TradeReport &t)
	{
		uint64_t rp = readPos.load(std::memory_order_relaxed);
		if (rp >= writePos.load(std::memory_order_acquire))
			return false;
		t = buffer[rp & (SIZE - 1)];
		readPos.store(rp + 1, std::memory_order_release);
		return true;
	}

	uint64_t size() const
	{
		return writePos.load(std::memory_order_acquire) - readPos.load(std::memory_order_acquire);
	}
};

// --- 3. STATISTICAL ORDER GENERATOR ---
class OrderGenerator
{
	std::mt19937_64 rng;
	std::normal_distribution<double> priceDist;
	std::uniform_real_distribution<double> uniform;
	std::uniform_int_distribution<int> qtyDist;

	uint64_t nextOrderId = 1;
	double currentCenter;
	const double priceStdDev;

public:
	OrderGenerator(uint64_t seed = 42, double centerPrice = 300.0, double stdDev = 50.0)
		: rng(seed), priceDist(centerPrice, stdDev), uniform(0.0, 1.0),
		  qtyDist(1, 100), currentCenter(centerPrice), priceStdDev(stdDev) {}

	struct GeneratedOrder
	{
		uint64_t id;
		Side side;
		OrderType type;
		uint32_t shares;
		int64_t price;
		int64_t stopPrice;
	};

	GeneratedOrder generateOrder(bool allowStop = true)
	{
		GeneratedOrder order;
		order.id = nextOrderId++;
		order.shares = qtyDist(rng);

		double r = uniform(rng);

		if (r < 0.50)
		{
			order.type = OrderType::Limit;
			order.side = (uniform(rng) < 0.5) ? Side::Buy : Side::Sell;
			double basePrice = priceDist(rng);
			if (order.side == Side::Buy)
				order.price = static_cast<int64_t>(std::max(1.0, basePrice - priceStdDev * 0.1));
			else
				order.price = static_cast<int64_t>(basePrice + priceStdDev * 0.1);
			order.stopPrice = 0;
		}
		else if (r < 0.80)
		{
			order.type = OrderType::Market;
			order.side = (uniform(rng) < 0.5) ? Side::Buy : Side::Sell;
			order.price = (order.side == Side::Buy) ? INT64_MAX : 0;
			order.stopPrice = 0;
		}
		else if (allowStop && r < 0.90)
		{
			order.type = OrderType::Stop;
			order.side = (uniform(rng) < 0.5) ? Side::Buy : Side::Sell;
			double basePrice = priceDist(rng);
			if (order.side == Side::Buy)
			{
				order.stopPrice = static_cast<int64_t>(basePrice + priceStdDev * 0.3);
				order.price = INT64_MAX;
			}
			else
			{
				order.stopPrice = static_cast<int64_t>(std::max(1.0, basePrice - priceStdDev * 0.3));
				order.price = 0;
			}
		}
		else if (allowStop)
		{
			order.type = OrderType::StopLimit;
			order.side = (uniform(rng) < 0.5) ? Side::Buy : Side::Sell;
			double basePrice = priceDist(rng);
			if (order.side == Side::Buy)
			{
				order.stopPrice = static_cast<int64_t>(basePrice + priceStdDev * 0.25);
				order.price = static_cast<int64_t>(basePrice + priceStdDev * 0.35);
			}
			else
			{
				order.stopPrice = static_cast<int64_t>(std::max(1.0, basePrice - priceStdDev * 0.25));
				order.price = static_cast<int64_t>(std::max(1.0, basePrice - priceStdDev * 0.35));
			}
		}
		else
		{
			order.type = OrderType::Limit;
			order.side = Side::Buy;
			order.price = static_cast<int64_t>(currentCenter);
			order.stopPrice = 0;
		}

		return order;
	}

	uint64_t getNextId() const { return nextOrderId; }
};

// --- 4. MEMORY ARENA ---
class MemoryManager
{
	std::vector<Order> oPool;
	std::vector<Limit> lPool;
	Order *fOrder;
	Limit *fLimit;

public:
	MemoryManager(size_t n) : oPool(n), lPool(n / 5)
	{
		for (size_t i = 0; i < oPool.size() - 1; ++i)
			oPool[i].nextFree = &oPool[i + 1];
		fOrder = &oPool[0];
		for (size_t i = 0; i < lPool.size() - 1; ++i)
			lPool[i].nextFree = &lPool[i + 1];
		fLimit = &lPool[0];
	}

	Order *getOrder(uint64_t i, Side s, OrderType t, uint32_t q, int64_t p, int64_t sp)
	{
		if (!fOrder)
			return nullptr;
		Order *o = fOrder;
		fOrder = fOrder->nextFree;
		o->id = i;
		o->side = s;
		o->type = t;
		o->shares = q;
		o->price = p;
		o->stopPrice = sp;
		o->next = o->prev = nullptr;
		o->parentLimit = nullptr;
		return o;
	}

	void recycleOrder(Order *o)
	{
		o->parentLimit = nullptr;
		o->prev = o->next = nullptr;
		o->nextFree = fOrder;
		fOrder = o;
	}

	Limit *getLimit(int64_t p)
	{
		if (!fLimit)
			return nullptr;
		Limit *l = fLimit;
		fLimit = fLimit->nextFree;
		l->price = p;
		l->height = 1;
		l->left = l->right = nullptr;
		l->head = l->tail = nullptr;
		return l;
	}

	void recycleLimit(Limit *l)
	{
		l->left = l->right = nullptr;
		l->head = l->tail = nullptr;
		l->nextFree = fLimit;
		fLimit = l;
	}
};

// --- 5. THE MATCHING ENGINE ---
class OrderBook
{
	MemoryManager &mm;
	Limit *buyRoot = nullptr, *sellRoot = nullptr;
	Limit *stopBuyRoot = nullptr, *stopSellRoot = nullptr;
	std::unordered_map<uint64_t, Order *> orderMap;
	std::unordered_map<uint64_t, Order *> stopOrderMap;
	RingBuffer<65536> &tradeBuffer;
	uint64_t timestampCounter = 0;
	uint64_t generatedIdCounter = 1000000000;

	int h(Limit *n) { return n ? n->height : 0; }
	void up(Limit *n) { n->height = 1 + std::max(h(n->left), h(n->right)); }
	int getBal(Limit *n) { return n ? h(n->left) - h(n->right) : 0; }

	Limit *rotR(Limit *y)
	{
		Limit *x = y->left;
		y->left = x->right;
		x->right = y;
		up(y);
		up(x);
		return x;
	}

	Limit *rotL(Limit *x)
	{
		Limit *y = x->right;
		x->right = y->left;
		y->left = x;
		up(x);
		up(y);
		return y;
	}

	Limit *insert(Limit *n, int64_t p, Limit *&target)
	{
		if (!n)
		{
			target = mm.getLimit(p);
			return target;
		}
		if (p < n->price)
			n->left = insert(n->left, p, target);
		else if (p > n->price)
			n->right = insert(n->right, p, target);
		else
		{
			target = n;
			return n;
		}
		up(n);
		int b = getBal(n);
		if (b > 1 && p < n->left->price)
			return rotR(n);
		if (b < -1 && p > n->right->price)
			return rotL(n);
		if (b > 1 && p > n->left->price)
		{
			n->left = rotL(n->left);
			return rotR(n);
		}
		if (b < -1 && p < n->right->price)
		{
			n->right = rotR(n->right);
			return rotL(n);
		}
		return n;
	}

	Limit *getMin(Limit *n) { return (n && n->left) ? getMin(n->left) : n; }
	Limit *getMax(Limit *n) { return (n && n->right) ? getMax(n->right) : n; }

	Limit *removeLimit(Limit *root, int64_t p)
	{
		if (!root)
			return nullptr;

		if (p < root->price)
			root->left = removeLimit(root->left, p);
		else if (p > root->price)
			root->right = removeLimit(root->right, p);
		else
		{
			if (!root->left || !root->right)
			{
				Limit *nodeToReturn = root->left ? root->left : root->right;
				mm.recycleLimit(root);
				return nodeToReturn;
			}

			Limit *successor = getMin(root->right);
			root->price = successor->price;
			root->head = successor->head;
			root->tail = successor->tail;

			Order *curr = root->head;
			while (curr)
			{
				curr->parentLimit = root;
				curr = curr->next;
			}

			root->right = removeLimit(root->right, successor->price);
		}

		if (!root)
			return nullptr;

		up(root);
		int b = getBal(root);
		if (b > 1 && getBal(root->left) >= 0)
			return rotR(root);
		if (b > 1 && getBal(root->left) < 0)
		{
			root->left = rotL(root->left);
			return rotR(root);
		}
		if (b < -1 && getBal(root->right) <= 0)
			return rotL(root);
		if (b < -1 && getBal(root->right) > 0)
		{
			root->right = rotR(root->right);
			return rotL(root);
		}
		return root;
	}

	void checkStopOrders(int64_t executedPrice, Side executedSide, std::vector<TriggeredStop> &triggered)
	{
		// Only check stops ONCE per order, not per trade
		if (executedSide == Side::Sell && stopSellRoot)
		{
			Limit *highest = getMax(stopSellRoot);
			while (highest && executedPrice <= highest->price)
			{
				Order *stopOrder = highest->head;
				while (stopOrder)
				{
					triggered.push_back({stopOrder->id, stopOrder->side,
										 (stopOrder->type == OrderType::Stop) ? OrderType::Market : OrderType::Limit,
										 stopOrder->shares, stopOrder->price});

					stopOrderMap.erase(stopOrder->id);
					Order *next = stopOrder->next;
					mm.recycleOrder(stopOrder);
					stopOrder = next;
				}

				int64_t priceToRemove = highest->price;
				stopSellRoot = removeLimit(stopSellRoot, priceToRemove);
				highest = getMax(stopSellRoot);
			}
		}

		if (executedSide == Side::Buy && stopBuyRoot)
		{
			Limit *lowest = getMin(stopBuyRoot);
			while (lowest && executedPrice >= lowest->price)
			{
				Order *stopOrder = lowest->head;
				while (stopOrder)
				{
					triggered.push_back({stopOrder->id, stopOrder->side,
										 (stopOrder->type == OrderType::Stop) ? OrderType::Market : OrderType::Limit,
										 stopOrder->shares, stopOrder->price});

					stopOrderMap.erase(stopOrder->id);
					Order *next = stopOrder->next;
					mm.recycleOrder(stopOrder);
					stopOrder = next;
				}

				int64_t priceToRemove = lowest->price;
				stopBuyRoot = removeLimit(stopBuyRoot, priceToRemove);
				lowest = getMin(stopBuyRoot);
			}
		}
	}

	void processOrderInternal(uint64_t id, Side side, OrderType type, uint32_t qty, int64_t price, int64_t stopPrice, bool checkStops)
	{
		if (type == OrderType::Stop || type == OrderType::StopLimit)
		{
			Order *stopOrder = mm.getOrder(id, side, type, qty, price, stopPrice);
			if (!stopOrder)
				return;

			Limit *L = nullptr;
			if (side == Side::Buy)
				stopBuyRoot = insert(stopBuyRoot, stopPrice, L);
			else
				stopSellRoot = insert(stopSellRoot, stopPrice, L);

			if (!L)
				return;

			if (!L->head)
				L->head = L->tail = stopOrder;
			else
			{
				L->tail->next = stopOrder;
				stopOrder->prev = L->tail;
				L->tail = stopOrder;
			}
			stopOrder->parentLimit = L;
			stopOrderMap[id] = stopOrder;
			return;
		}

		Order *taker = mm.getOrder(id, side, type, qty, price, stopPrice);
		if (!taker)
			return;

		std::vector<TriggeredStop> triggeredStops;
		int64_t lastExecutedPrice = 0;

		while (taker->shares > 0)
		{
			Limit *best = (side == Side::Buy) ? getMin(sellRoot) : getMax(buyRoot);
			if (!best || (side == Side::Buy && price < best->price) || (side == Side::Sell && price > best->price))
				break;

			Order *maker = best->head;
			while (maker && taker->shares > 0)
			{
				uint32_t traded = std::min(taker->shares, maker->shares);

				tradeBuffer.push({taker->id, maker->id, traded, best->price, timestampCounter++});
				lastExecutedPrice = best->price;

				taker->shares -= traded;
				maker->shares -= traded;

				if (maker->shares == 0)
				{
					best->head = maker->next;
					if (best->head)
						best->head->prev = nullptr;
					else
						best->tail = nullptr;
					orderMap.erase(maker->id);
					mm.recycleOrder(maker);
					maker = best->head;
				}
				else
					break;
			}

			if (!best->head)
			{
				if (side == Side::Buy)
					sellRoot = removeLimit(sellRoot, best->price);
				else
					buyRoot = removeLimit(buyRoot, best->price);
			}
		}

		// Check stops ONCE after all matching completes
		if (checkStops && lastExecutedPrice > 0)
			checkStopOrders(lastExecutedPrice, side, triggeredStops);

		if (taker->shares > 0 && type == OrderType::Limit)
		{
			Limit *L = nullptr;
			if (side == Side::Buy)
				buyRoot = insert(buyRoot, price, L);
			else
				sellRoot = insert(sellRoot, price, L);

			if (!L)
				return;

			if (!L->head)
				L->head = L->tail = taker;
			else
			{
				L->tail->next = taker;
				taker->prev = L->tail;
				L->tail = taker;
			}
			taker->parentLimit = L;
			orderMap[id] = taker;
		}
		else
		{
			mm.recycleOrder(taker);
		}

		// Process triggered stops
		for (const auto &ts : triggeredStops)
		{
			uint64_t newId = generatedIdCounter++;
			processOrderInternal(newId, ts.side, ts.convertToType, ts.shares, ts.limitPrice, 0, false);
		}
	}

public:
	OrderBook(MemoryManager &m, RingBuffer<65536> &rb) : mm(m), tradeBuffer(rb) {}

	void processOrder(uint64_t id, Side side, OrderType type, uint32_t qty, int64_t price, int64_t stopPrice)
	{
		processOrderInternal(id, side, type, qty, price, stopPrice, true);
	}

	bool cancelOrder(uint64_t orderId)
	{
		auto it = orderMap.find(orderId);
		if (it != orderMap.end())
		{
			Order *o = it->second;
			Limit *L = o->parentLimit;

			if (o->prev)
				o->prev->next = o->next;
			else
				L->head = o->next;

			if (o->next)
				o->next->prev = o->prev;
			else
				L->tail = o->prev;

			if (!L->head)
			{
				if (o->side == Side::Buy)
					buyRoot = removeLimit(buyRoot, L->price);
				else
					sellRoot = removeLimit(sellRoot, L->price);
			}

			orderMap.erase(it);
			mm.recycleOrder(o);
			return true;
		}

		auto stopIt = stopOrderMap.find(orderId);
		if (stopIt != stopOrderMap.end())
		{
			Order *o = stopIt->second;
			Limit *L = o->parentLimit;

			if (o->prev)
				o->prev->next = o->next;
			else
				L->head = o->next;

			if (o->next)
				o->next->prev = o->prev;
			else
				L->tail = o->prev;

			if (!L->head)
			{
				if (o->side == Side::Buy)
					stopBuyRoot = removeLimit(stopBuyRoot, L->price);
				else
					stopSellRoot = removeLimit(stopSellRoot, L->price);
			}

			stopOrderMap.erase(stopIt);
			mm.recycleOrder(o);
			return true;
		}

		return false;
	}

	bool modifyOrder(uint64_t orderId, uint32_t newQty, int64_t newPrice)
	{
		auto it = orderMap.find(orderId);
		if (it == orderMap.end())
			return false;

		Order *o = it->second;

		if (newPrice == o->price)
		{
			o->shares = newQty;
			return true;
		}

		Limit *oldLimit = o->parentLimit;
		Side side = o->side;

		if (o->prev)
			o->prev->next = o->next;
		else
			oldLimit->head = o->next;

		if (o->next)
			o->next->prev = o->prev;
		else
			oldLimit->tail = o->prev;

		if (!oldLimit->head)
		{
			if (side == Side::Buy)
				buyRoot = removeLimit(buyRoot, oldLimit->price);
			else
				sellRoot = removeLimit(sellRoot, oldLimit->price);
		}

		o->price = newPrice;
		o->shares = newQty;
		o->prev = o->next = nullptr;

		Limit *newLimit = nullptr;
		if (side == Side::Buy)
			buyRoot = insert(buyRoot, newPrice, newLimit);
		else
			sellRoot = insert(sellRoot, newPrice, newLimit);

		if (!newLimit)
			return false;

		if (!newLimit->head)
			newLimit->head = newLimit->tail = o;
		else
		{
			newLimit->tail->next = o;
			o->prev = newLimit->tail;
			newLimit->tail = o;
		}
		o->parentLimit = newLimit;

		return true;
	}

	size_t getOrderCount() const { return orderMap.size(); }
	size_t getStopOrderCount() const { return stopOrderMap.size(); }
};

// --- 6. BENCHMARK SUITE ---
void runBenchmark(const char *name, OrderBook &engine,
				  std::function<void(int)> testFunc,
				  int testSize, RingBuffer<65536> &tradeBuffer)
{
	std::cout << "\n=== " << name << " ===" << std::endl;

	auto start = std::chrono::high_resolution_clock::now();
	testFunc(testSize);
	auto end = std::chrono::high_resolution_clock::now();

	std::chrono::duration<double> diff = end - start;

	std::cout << "Throughput: " << (testSize / diff.count()) / 1e6 << " Million TPS" << std::endl;
	std::cout << "Regular Orders in Book: " << engine.getOrderCount() << std::endl;
	std::cout << "Stop Orders in Book: " << engine.getStopOrderCount() << std::endl;
	std::cout << "Trades Pending: " << tradeBuffer.size() << std::endl;
}

int main()
{
	const int TEST_SIZE = 1000000;
	MemoryManager mm(TEST_SIZE * 3);
	RingBuffer<65536> tradeBuffer;

	std::atomic<bool> running{true};
	std::atomic<uint64_t> totalTrades{0};

	std::thread consumer([&]()
						 {
        TradeReport t;
        while (running.load(std::memory_order_relaxed)) {
            if (tradeBuffer.pop(t))
                totalTrades.fetch_add(1, std::memory_order_relaxed);
            else
                std::this_thread::yield();
        }
        while (tradeBuffer.pop(t))
            totalTrades.fetch_add(1, std::memory_order_relaxed); });

	OrderBook engine(mm, tradeBuffer);
	OrderGenerator generator(42, 300.0, 50.0);

	std::cout << "Starting Fixed Matching Engine..." << std::endl;

	runBenchmark("Test 1: Statistical Orders", engine, [&](int n)
				 {
        for (int i = 0; i < 10000; ++i) {
            auto order = generator.generateOrder(false);
            engine.processOrder(order.id, order.side, OrderType::Limit, 
                              order.shares, order.price, 0);
        }

        for (int i = 0; i < n; ++i) {
            auto order = generator.generateOrder(true);
            engine.processOrder(order.id, order.side, order.type,
                              order.shares, order.price, order.stopPrice);
            
            if (i > 100 && i % 7 == 0)
                engine.cancelOrder(order.id - (rand() % 50 + 10));
        } }, TEST_SIZE, tradeBuffer);

	std::vector<uint64_t> activeOrders;
	runBenchmark("Test 2: Order Modification", engine, [&](int n)
				 {
        uint64_t baseId = generator.getNextId();
        
        for (int i = 0; i < n; ++i) {
            if (i % 3 == 0) {
                engine.processOrder(baseId + i, Side::Buy, OrderType::Limit, 10, 300 + (i % 10), 0);
                activeOrders.push_back(baseId + i);
            } else if (i % 3 == 1 && !activeOrders.empty()) {
                engine.modifyOrder(activeOrders[i % activeOrders.size()], 15, 300 + (i % 15));
            } else if (!activeOrders.empty()) {
                engine.cancelOrder(activeOrders.back());
                activeOrders.pop_back();
            }
        } }, TEST_SIZE, tradeBuffer);

	runBenchmark("Test 3: Mixed Workload", engine, [&](int n)
				 {
        for (int i = 0; i < n; ++i) {
            auto order = generator.generateOrder(true);
            
            double r = static_cast<double>(rand()) / RAND_MAX;
            if (r < 0.75) {
                engine.processOrder(order.id, order.side, order.type,
                                  order.shares, order.price, order.stopPrice);
            } else if (r < 0.90) {
                engine.cancelOrder(order.id - 100);
            } else {
                engine.modifyOrder(order.id - 50, order.shares + 5, order.price + 1);
            }
        } }, TEST_SIZE, tradeBuffer);

	running.store(false, std::memory_order_relaxed);
	consumer.join();

	std::cout << "\n=== FINAL RESULTS ===" << std::endl;
	std::cout << "Total Trades Executed: " << totalTrades.load() << std::endl;
	std::cout << "Regular Orders Remaining: " << engine.getOrderCount() << std::endl;
	std::cout << "Stop Orders Remaining: " << engine.getStopOrderCount() << std::endl;

	return 0;
}
