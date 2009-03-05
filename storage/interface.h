#ifndef STORAGE_INTERFACE_H__
#define STORAGE_INTERFACE_H__

#include "storage.h"
#include "storage/buffer_queue.h"
#include "logic/clock.h"
#include <mp/pthread.h>
#include <mp/shared_buffer.h>
#include <stdint.h>
#include <msgpack.hpp>
#include <arpa/inet.h>

#ifdef __LITTLE_ENDIAN__
#if defined(__bswap_64)
#  define kumo_be64(x) __bswap_64(x)
#elif defined(__DARWIN_OSSwapInt64)
#  define kumo_be64(x) __DARWIN_OSSwapInt64(x)
#else
static inline uint64_t kumo_be64(uint64_t x) {
	return	((x << 56) & 0xff00000000000000ULL ) |
			((x << 40) & 0x00ff000000000000ULL ) |
			((x << 24) & 0x0000ff0000000000ULL ) |
			((x <<  8) & 0x000000ff00000000ULL ) |
			((x >>  8) & 0x00000000ff000000ULL ) |
			((x >> 24) & 0x0000000000ff0000ULL ) |
			((x >> 40) & 0x000000000000ff00ULL ) |
			((x >> 56) & 0x00000000000000ffULL ) ;
}
#endif
#else
#define kumo_be64(x) (x)
#endif

/* Big endian
 *
 * key:
 * +--------+-----------------+
 * |   64   |       ...       |
 * +--------+-----------------+
 * hash
 *          key
 *
 * value:
 * +--------+--------+-----------------+
 * |   64   |   64   |       ...       |
 * +--------+--------+-----------------+
 * clocktime
 *          meta
 *                   data
 *
 * value (garbage):
 * +--------+
 * |   64   |
 * +--------+
 * clocktime
 */

namespace kumo {


class Storage {
public:
	Storage(const char* path,
			uint32_t garbage_min_time,
			uint32_t garbage_max_time,
			size_t garbage_mem_limit);

	~Storage();

	static const size_t KEY_META_SIZE = 8;
	static const size_t VALUE_CLOCKTIME_SIZE = 8;
	static const size_t VALUE_META_SIZE = VALUE_CLOCKTIME_SIZE + 8;


	static ClockTime clocktime_of(const char* raw_val);
	static void clocktime_to(ClockTime clocktime, char* raw_val);

	static uint64_t meta_of(const char* raw_val);
	static void meta_to(uint64_t meta, char* raw_val);

	static uint64_t hash_of(const char* raw_key);
	static void hash_to(uint64_t hash, char* raw_key);

public:
	const char* get(
			const char* raw_key, uint32_t raw_keylen,
			uint32_t* result_raw_vallen, msgpack::zone* z);

	void set(
			const char* raw_key, uint32_t raw_keylen,
			const char* raw_val, uint32_t raw_vallen);

	bool update(
			const char* raw_key, uint32_t raw_keylen,
			const char* raw_val, uint32_t raw_vallen);

	bool remove(
			const char* raw_key, uint32_t raw_keylen,
			ClockTime update_clocktime);

	// FIXME
	//void updatev()

	uint64_t rnum();

	void backup(const char* dstpath);

	std::string error();

	template <typename F>
	void for_each(F f, ClockTime clocktime);

	struct iterator {
	public:
		iterator(kumo_storage_op* op, void* data);
		~iterator();

	public:
		const char* key();
		const char* val();
		size_t keylen();
		size_t vallen();
		void release_key(msgpack::zone* z);
		void release_val(msgpack::zone* z);
		void del();

	private:
		void* m_data;
		kumo_storage_op* m_op;
	};

private:
	void* m_data;
	kumo_storage_op m_op;

	mp::pthread_mutex m_garbage_mutex;
	buffer_queue m_garbage;

	uint32_t m_garbage_min_time;
	uint32_t m_garbage_max_time;
	size_t m_garbage_mem_limit;

private:
	template <typename F>
	static void for_each_callback(void* obj, iterator& it);

	void for_each_impl(void* obj, void (*callback)(void* obj, iterator& it),
			ClockTime clocktime);
};


inline ClockTime Storage::clocktime_of(const char* raw_val)
{
	return ClockTime( kumo_be64(*(uint64_t*)raw_val) );
}

inline void Storage::clocktime_to(ClockTime clocktime, char* raw_val)
{
	*(uint64_t*)raw_val = kumo_be64(clocktime.get());
}

inline uint64_t Storage::meta_of(const char* raw_val)
{
	return kumo_be64(*(uint64_t*)(raw_val+8));
}

inline void Storage::meta_to(uint64_t meta, char* raw_val)
{
	*((uint64_t*)(raw_val+8)) = kumo_be64(meta);
}

inline uint64_t Storage::hash_of(const char* raw_key)
{
	return kumo_be64(*(uint64_t*)raw_key);
}

inline void Storage::hash_to(uint64_t hash, char* raw_key)
{
	*(uint64_t*)raw_key = kumo_be64(hash);
}


inline const char* Storage::get(
		const char* raw_key, uint32_t raw_keylen,
		uint32_t* result_raw_vallen, msgpack::zone* z)
{
	const char* raw_val = m_op.get(m_data,
			raw_key, raw_keylen,
			result_raw_vallen,
			z);
	if(raw_val && *result_raw_vallen < VALUE_META_SIZE) {
		return NULL;
	}
	return raw_val;
}


template <typename F>
inline void Storage::for_each(F f, ClockTime clocktime)
{
	for_each_impl(
			reinterpret_cast<void*>(&f),
			&Storage::for_each_callback<F>,
			clocktime);
}

template <typename F>
void Storage::for_each_callback(void* obj, iterator& it)
{
	(*reinterpret_cast<F*>(obj))(it);
}


inline Storage::iterator::iterator(kumo_storage_op* op, void* data) :
	m_data(data), m_op(op) { }

inline Storage::iterator::~iterator() { }

inline const char* Storage::iterator::key()
{
	return m_op->iterator_key(m_data);
}

inline const char* Storage::iterator::val()
{
	return m_op->iterator_val(m_data);
}

inline size_t Storage::iterator::keylen()
{
	return m_op->iterator_keylen(m_data);
}

inline size_t Storage::iterator::vallen()
{
	return m_op->iterator_vallen(m_data);
}

inline void Storage::iterator::release_key(msgpack::zone* z)
{
	if(!m_op->iterator_release_key(m_data, z)) {
		throw std::bad_alloc();
	}
}

inline void Storage::iterator::release_val(msgpack::zone* z)
{
	if(!m_op->iterator_release_val(m_data, z)) {
		throw std::bad_alloc();
	}
}

inline void Storage::iterator::del()
{
	m_op->iterator_del_force(m_data);
}


}  // namespace kumo

#endif /* storage/storage.h */
