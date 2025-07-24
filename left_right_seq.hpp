#include <atomic>
#include <type_traits>
// The type T must be safe to read while another thread modifies it,
// for example all of its members are atomic.
template<typename T>
class left_right_seq {
private:
	T instances[2];
	/*
		Here are possible states of last two bits of counter:
		00 both instances are ready, but [0] is the latest one to use
		01 the [1] is under construction, so use [0]
		10 (DOES NOT HAPPEN) both instances are ready, but [1] is the latest one to use
		11 the [0] is under construction, so use [1]
	*/
	std::atomic<uint64_t> counter{ 0 };
public:
	left_right_seq() = default;

	explicit left_right_seq(const T& initial_value)
		: instances{ initial_value, initial_value } {
	}

	left_right_seq(const left_right_seq& other) : left_right_seq(other.load()) {}

	left_right_seq& operator=(const left_right_seq& other) {
		if (this != &other) {
			store(other.load());
		}
		return *this;
	}

	// Write operation with visitor lambda op, which will be passed &T.
	// The caller should synchronize with other writes by some other means, so there is just one write at a time.
	// The op will be called on both instances, and should do equivalent thing to both.
	// The op can use std::memory_order_relaxed store()s to modify T.
	template<typename OP>
	auto write(OP&& op) -> decltype(op(std::declval<T&>())) {
		const uint64_t c = counter.load(std::memory_order_relaxed);

		counter.store(c + 1, std::memory_order_relaxed);
		std::atomic_thread_fence(std::memory_order_release);
		op(instances[1]);

		counter.store(c + 3, std::memory_order_release);

		std::atomic_thread_fence(std::memory_order_release);
		if constexpr (std::is_void_v<decltype(op(std::declval<T&>()))>) {
			op(instances[0]);
			counter.store(c + 4, std::memory_order_release);
		}
		else {
			const auto result = op(instances[0]);
			counter.store(c + 4, std::memory_order_release);
			return result;
		}
	}

	// Read operation with visitor lambda op, which will be passed const T&.
	// The op must not crash even if another thread modifies T in parallel.
	// The op might be called multiple times due to retries, and the first successful result will be returned.
	// The op can use std::memory_order_relaxed load()s to access T.
	template<typename OP>
	auto read(OP&& op) const -> decltype(op(std::declval<const T&>())) {
		while (true) {
			const auto c_before = counter.load(std::memory_order_acquire);
			const int instance_idx = (c_before >> 1) & 1;
			const auto result = op(instances[instance_idx]);
			// Acquire fence after calling visitor
			std::atomic_thread_fence(std::memory_order_acquire);
			// if any of loads in op() saw a "new" state, then the load() below will see the new counter value
			const auto c_after = counter.load(std::memory_order_relaxed);
			if ((c_after >> 1) == (c_before >> 1)) {
				return result;
			}
		}
	}

	T load() const {
		return read([](const T& value) { return value; });
	}

	void store(const T& value) {
		write([&value](T& instance) { instance = value; });
	}

	operator T() const {
		return load();
	}

	left_right_seq& operator=(const T& value) {
		store(value);
		return *this;
	}
};