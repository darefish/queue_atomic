//
//  queue_atomic_v3.h
//

#ifndef queue_atomic_v3_h
#define queue_atomic_v3_h

/*
 * queue_atomic_v3
 *
 *   - uses 4 atomic variables: version_counter_back, version_back, version_counter_front and version_front
 *
 *   - push_back reads 3 atomics: version_counter_back, version_back and version_front
 *               writes 2 atomics: version_counter_back and version_back
 *
 *   - pop_front reads 3 atomics: version_counter_front, version_back and version_front
 *               writes 2 atomics: version_counter_front and version_front
 *
 *   - back version and front version are packed into version_back and version_front
 *
 *   - version is used for conflict detection during ordered writes
 *
 */

template <typename T,
        const int debug_contention = false,
        typename ATOMIC_UINT = uint64_t,
        const int OFFSET_BITS = 32,
        const int VERSION_BITS = 32,
        std::memory_order relaxed_memory_order = std::memory_order_relaxed,
        std::memory_order acquire_memory_order = std::memory_order_acquire,
        std::memory_order release_memory_order = std::memory_order_release>
struct queue_atomic_v3
{
    /* queue atomic type */
    
    typedef ATOMIC_UINT                         atomic_uint_t;
    typedef std::atomic<T>                      atomic_item_t;
    
    
    /* queue constants */
    
    static const int tight_spin_limit =         8;
    static const int spin_limit =               1 << 24;
    static const int debug_spin =               true;
    static const int atomic_bits =              sizeof(atomic_uint_t) << 3;
    static const int offset_bits =              OFFSET_BITS;
    static const int version_bits =             VERSION_BITS;
    static const int offset_shift =             0;
    static const int version_shift =            offset_bits;
    static const atomic_uint_t size_max =       (1ULL << (offset_bits - 1));
    static const atomic_uint_t offset_limit =   (1ULL << offset_bits);
    static const atomic_uint_t version_limit =  (1ULL << version_bits);
    static const atomic_uint_t offset_mask =    (1ULL << offset_bits) - 1;
    static const atomic_uint_t version_mask =   (1ULL << version_bits) - 1;
    
    
    /* queue storage */
    
    atomic_item_t *vec;
    const atomic_uint_t size_limit;
    std::atomic<atomic_uint_t> version_counter_back __attribute__ ((aligned (64)));
    std::atomic<atomic_uint_t> version_back;
    std::atomic<atomic_uint_t> version_counter_front __attribute__ ((aligned (64)));
    std::atomic<atomic_uint_t> version_front;
    
    
    /* queue helpers */
    
    static inline bool ispow2(size_t val) { return val && !(val & (val-1)); }
    
    static inline const atomic_uint_t pack_offset(const atomic_uint_t version, const atomic_uint_t offset)
    {
        assert(version < version_limit);
        assert(offset < offset_limit);
        return (version << version_shift) | (offset << offset_shift);
    }
    
    static inline bool unpack_back_offsets(const atomic_uint_t version_counter_back, const atomic_uint_t back_pack,
                                           atomic_uint_t &back_offset)
    {
        if (((back_pack >> version_shift) & version_mask) == (version_counter_back & version_mask)) {
            back_offset = (back_pack >> offset_shift) & offset_mask;
            return true;
        }
        return false;
    }

    static inline bool unpack_front_offsets(const atomic_uint_t version_counter_front, const atomic_uint_t front_pack,
                                            atomic_uint_t &front_offset)
    {
        if (((front_pack >> version_shift) & version_mask) == (version_counter_front & version_mask)) {
            front_offset = (front_pack >> offset_shift) & offset_mask;
            return true;
        }
        return false;
    }
    
    /* queue implementation */
    
    atomic_uint_t _back_version()   { return (version_back >> version_shift) & version_mask; }
    atomic_uint_t _front_version()  { return (version_front >> version_shift) & version_mask; }
    atomic_uint_t _back()           { return (version_back >> offset_shift) & offset_mask; }
    atomic_uint_t _front()          { return (version_front >> offset_shift) & offset_mask; }
    size_t capacity()               { return size_limit; }
    
    
    queue_atomic_v3(size_t size_limit) :
        size_limit(size_limit),
        version_counter_back(0),
        version_back(pack_offset(0, 0)),
        version_counter_front(0),
        version_front(pack_offset(0, size_limit))
    {
        static_assert(version_bits + offset_bits <= atomic_bits,
                      "version_bits + offset_bits must fit into atomic integer type");
        assert(size_limit > 0);
        assert(size_limit <= size_max);
        assert(ispow2(size_limit));
        vec = new atomic_item_t[size_limit]();
        assert(vec != nullptr);
    }
    
    virtual ~queue_atomic_v3()
    {
        delete [] vec;
    }
    
    bool empty()
    {
        atomic_uint_t back = (version_back >> offset_shift) & offset_mask;
        atomic_uint_t front = (version_front >> offset_shift) & offset_mask;
        
        // return true if queue is empty
        return (front - back == size_limit);
    }
    
    bool full()
    {
        atomic_uint_t back = (version_back >> offset_shift) & offset_mask;
        atomic_uint_t front = (version_front >> offset_shift) & offset_mask;

        // return true if queue is full
        return (front == back);
    }
    
    size_t size()
    {
        atomic_uint_t back = (version_back >> offset_shift) & offset_mask;
        atomic_uint_t front = (version_front >> offset_shift) & offset_mask;

        // return queue size
        return (front < back) ? back - front : size_limit - front + back;
    }
    
    bool push_back(T elem)
    {
        atomic_uint_t back;
        atomic_uint_t front = (version_front >> offset_shift) & offset_mask;

        int spin_count = 0;
        do {
            // if back_version equals last_back_version then attempt push back
            atomic_uint_t _version_counter_back = version_counter_back.load(relaxed_memory_order);
            atomic_uint_t _version_back = version_back.load(relaxed_memory_order);
            if (unpack_back_offsets(_version_counter_back, _version_back, back))
            {
                // if (full) return false;
                if (front == back) return false;
                
                // increment back_version
                atomic_uint_t new_back_version = (_version_counter_back + 1) & version_mask;
                
                // calculate store offset and update back
                size_t offset = back++ & (size_limit - 1);
                
                // pack back_version and back
                atomic_uint_t pack = pack_offset(new_back_version, back & (offset_limit - 1));
                
                // compare_exchange_weak to attempt to update the version_counter
                // if successful other threads will spin until new version_back is visible
                // if successful then write value followed by version_back
                if (version_counter_back.compare_exchange_weak(_version_counter_back, new_back_version, std::memory_order_acq_rel)) {
                    vec[offset].store(elem, release_memory_order);
                    version_back.store(pack, release_memory_order);
                    return true;
                } else if (debug_contention) {
                    uint64_t _tsc = rdtsc();
                    log_debug("%s version=%llu time=%llu spin_count=%d thread:%p phase 2 contention",
                              __func__, _version_counter_back, _tsc, spin_count, std::this_thread::get_id());
                }
            } else {
                if (debug_contention) {
                    uint64_t _tsc = rdtsc();
                    log_debug("%s version=%llu time=%llu spin_count=%d thread:%p phase 1 contention",
                              __func__, _version_counter_back, _tsc, spin_count, std::this_thread::get_id());
                }
            }
            
            // yield the thread before retrying
            if (spin_limit > tight_spin_limit) {
                std::this_thread::yield();
            }
            
        } while (++spin_count < spin_limit);
        
        if (debug_spin) {
            log_debug("%s thread:%p failed: reached spin limit", __func__, std::this_thread::get_id());
        }
        
        return false;
    }
    
    T pop_front()
    {
        atomic_uint_t back = (version_back >> offset_shift) & offset_mask;
        atomic_uint_t front;
        
        int spin_count = 0;
        do {
            // if front_version equals last_front_version then attempt pop front
            atomic_uint_t _version_counter_front = version_counter_front.load(relaxed_memory_order);
            atomic_uint_t _version_front = version_front.load(relaxed_memory_order);
            if (unpack_front_offsets(_version_counter_front, _version_front, front))
            {
                // if (empty) return nullptr;
                if (front - back == size_limit) return T(0);
                
                // increment front_version
                atomic_uint_t new_front_version = (_version_counter_front + 1) & version_mask;
                
                // calculate offset and update front
                size_t offset = front++ & (size_limit - 1);
                
                // pack front_version and front
                atomic_uint_t pack = pack_offset(new_front_version, front & (offset_limit - 1));
                
                // compare_exchange_weak to attempt to update the version_counter
                // if successful other threads will spin until new version_front is visible
                // if successful then write version_front
                if (version_counter_front.compare_exchange_weak(_version_counter_front, new_front_version, std::memory_order_acq_rel)) {
                    T val = vec[offset].load(acquire_memory_order);
                    version_front.store(pack, release_memory_order);
                    return val;
                } else if (debug_contention) {
                    uint64_t _tsc = rdtsc();
                    log_debug("%s version=%llu time=%llu spin_count=%d thread:%p phase 2 contention",
                              __func__, _version_counter_front, _tsc, spin_count, std::this_thread::get_id());
                }
            } else {
                if (debug_contention) {
                    uint64_t _tsc = rdtsc();
                    log_debug("%s version=%llu time=%llu spin_count=%d thread:%p phase 1 contention",
                              __func__, _version_counter_front, _tsc, spin_count, std::this_thread::get_id());
                }
            }
            
            // yield the thread before retrying
            if (spin_limit > tight_spin_limit) {
                std::this_thread::yield();
            }
            
        } while (++spin_count < spin_limit);
        
        if (debug_spin) {
            log_debug("%s thread:%p failed: reached spin limit", __func__, std::this_thread::get_id());
        }
        
        return T(0);
    }
};

#endif
