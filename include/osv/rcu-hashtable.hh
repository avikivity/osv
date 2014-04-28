/*
 * Copyright (C) 2014 Cloudius Systems, Ltd.
 *
 * This work is open source software, licensed under the terms of the
 * BSD license as described in the LICENSE file in the top-level directory.
 */

#ifndef RCU_HASHTABLE_HH_
#define RCU_HASHTABLE_HH_

#include <osv/rcu-list.hh>
#include <vector>
#include <functional>
#include <memory>
#include <osv/sched.hh>

namespace osv {

namespace detail {

template <typename T, typename Hash = std::hash<T>, typename Compare = std::equal_to<T>>
class rcu_hashtable {
private:
    using value_type = T;
    using bucket_type = rcu_list<T>;
    struct bucket_array_type : std::vector<bucket_type> {
        explicit bucket_array_type(size_t n = 1) : std::vector<bucket_type>(n) {}
        size_t total_elements = 0;
    };
    using bucket_array_ptr_type = rcu_ptr<bucket_array_type, rcu_deleter<bucket_array_type>>;
    // ugly workaround for sizeof(empty struct) != 0
    struct bucket_array_ptr_plus_hash_plus_compare : private Hash, private Compare {
        bucket_array_ptr_plus_hash_plus_compare(bucket_array_type* ptr,
                const Hash& hash,
                const Compare& compare)
            : Hash(hash), Compare(compare), _ptr(ptr) {}
        bucket_array_ptr_type& ptr() { return _ptr; }
        Hash& hash() { return *this; }
        Compare& compare() { return *this; }
        bucket_array_ptr_type _ptr;
    };
private:
    bucket_array_ptr_plus_hash_plus_compare _ba_hash_compare;
    bucket_array_ptr_type& ptr() { return _ba_hash_compare.ptr(); }
    Hash& hash() { return _ba_hash_compare.hash(); }
    Compare& compare() { return _ba_hash_compare.compare(); }
public:
    class read_only_table;
    class mutable_table;
public:
    rcu_hashtable(const Hash& hash = Hash(), const Compare& compare = Compare())
        : _ba_hash_compare(new bucket_array_type, hash, compare) {}
    read_only_table for_read();
    mutable_table by_owner();
private:
    friend read_only_table;
    friend mutable_table;
};

template <typename T, typename Hash, typename Compare>
class rcu_hashtable<T, Hash, Compare>::read_only_table {
private:
    rcu_hashtable& _table;
    bucket_array_type& _buckets;
public:
    class iterator;
    friend iterator;
    class iterator {
    private:
        typename bucket_array_type::iterator _which_bucket;
        typename rcu_list<T>::read_only_list::iterator _in_bucket;
    public:
        iterator(typename bucket_array_type::iterator which_bucket,
                 typename rcu_list<T>::read_only_list::iterator in_bucket)
            : _which_bucket(which_bucket), _in_bucket(in_bucket) {}
        T& operator*() const { return *_in_bucket; }
        T* operator->() const { return &*_in_bucket; }
        iterator& operator++() {
            if (++_in_bucket ==  bucket_type::read_only_list::end_iterator()) {
                ++_which_bucket;
                _in_bucket = _which_bucket->for_read().begin();
            }
            return *this;
        }
        iterator& operator++(int) {
            auto old = *this;
            ++*this;
            return old;
        }
        bool operator==(const iterator& x) const {
            return _in_bucket == x._in_bucket
                    && _which_bucket == x._which_bucket;
        }
        bool operator!=(const iterator& x) const {
            return !operator==(x);
        }
    };
public:
    explicit read_only_table(rcu_hashtable& h) : _table(h), _buckets(*h.ptr().read()) {}
    iterator begin() {
        auto bucket = _buckets.begin();
        if (bucket != _buckets.end()) {
            auto list = bucket->for_read(); // must happen just once
            return { bucket, list.begin() };
        } else {
            return { bucket, bucket_type::read_only_list::end_iterator() };
        }
    }
    iterator end() {
        return { _buckets.end(), rcu_list<T>::read_only_list::end_iterator() };
    }
    iterator find(const T& data) { return find(data, _table.hash(), _table.compare()); }
    template <typename Key, typename KeyHash, typename KeyValueCompare>
    iterator find(Key&& key, KeyHash&& hash, KeyValueCompare&& compare);
};

template <typename T, typename Hash, typename Compare>
template <typename Key, typename KeyHash, typename KeyValueCompare>
auto rcu_hashtable<T, Hash, Compare>::read_only_table::find(Key&& key, KeyHash&& hash, KeyValueCompare&& compare) -> iterator
{
    auto bucket = _buckets.begin() + (hash(key) & (_buckets.size() - 1));
    auto list = bucket->for_read();
    for (auto i = list.begin(); i != list.end(); ++i) {
        if (compare(key, *i)) {
            return iterator(bucket, i);
        }
    }
    return iterator(_buckets.end(), list.end_iterator());
}

template <typename T, typename Hash, typename Compare>
class rcu_hashtable<T, Hash, Compare>::mutable_table {
private:
    rcu_hashtable& _table;
public:
    class iterator;
    friend iterator;
    class iterator {
    private:
        typename bucket_array_type::iterator _which_bucket;
        typename bucket_type::mutable_list::iterator _in_bucket;
    public:
        iterator(mutable_table& mht,
                 typename bucket_array_type::iterator which_bucket,
                 typename bucket_type::mutable_list::iterator in_bucket)
            : _mht(mht), _which_bucket(which_bucket), _in_bucket(in_bucket) {
            skip_empty();
        }
        T& operator*() const { return *_in_bucket; }
        T* operator->() const { return &*_in_bucket; }
        iterator& operator++() {
            ++_in_bucket;
            skip_empty();
            return *this;
        }
        iterator& operator++(int) {
            auto old = *this;
            ++*this;
            return old;
        }
        bool operator==(const iterator& x) const {
            return _in_bucket == x._in_bucket
                    && _which_bucket == x._which_bucket;
        }
        bool operator!=(const iterator& x) const {
            return !operator==(x);
        }
    private:
        void skip_empty() {
            while (_in_bucket == _which_bucket->by_owner() && _which_bucket != _ht.end()) {
                ++_which_bucket;
                _in_bucket = _which_bucket->by_owner().begin();
            }
        }
        mutable_table& _mht;
        friend mutable_table;
    };
public:
    explicit mutable_table(rcu_hashtable& h) : _table(h) {}
    iterator begin() {
        auto bucket = _table.ptr().read_by_owner()->begin();
        if (bucket != _table.ptr().read_by_owner()->end()) {
            auto list = bucket->by_owner();
            return { bucket, list.begin() };
        } else {
            return { bucket, bucket_type::mutable_list::end_iterator() };
        }
    }
    iterator end() {
        return { _table.ptr().read_by_owner()->end(), bucket_type::mutable_list::end_iterator() };
    }
    template <typename Key, typename KeyHash, typename KeyValueCompare>
    iterator find(Key&& key, KeyHash&& hash, KeyValueCompare&& compare);
    void erase(iterator i);
    void push_front(const T& data);
    template <typename... Args>
    void emplace_front(Args&&... args);
    void insert_before(iterator i, const T& data);
    template <typename... Args>
    void emplace_before(iterator i, Args&&... args);
private:
    void increase_size_maybe_expand();
    void decrease_size_maybe_contract();
    void rebuild(size_t new_size);
};

template <typename T, typename Hash, typename Compare>
template <typename Key, typename KeyHash, typename KeyValueCompare>
auto rcu_hashtable<T, Hash, Compare>::mutable_table::find(Key&& key, KeyHash&& hash, KeyValueCompare&& compare) -> iterator
{
    bucket_array_type& buckets = *_table.ptr().read_by_owner();
    auto bucket = buckets.begin() + (hash(key) & (buckets.size() - 1));
    auto list = bucket->by_owner();
    for (auto i = list.begin(); i != list.end(); ++i) {
        if (compare(key, *i)) {
            return iterator(bucket, i);
        }
    }
    return end();
}

template <typename T, typename Hash, typename Compare>
template <typename... Args>
void rcu_hashtable<T, Hash, Compare>::mutable_table::emplace_front(Args&&... args)
{
    increase_size_maybe_expand();
    bucket_array_type& buckets = *_table.ptr().read_by_owner();
    rcu_list<T> tmp;
    auto mtmp = tmp.by_owner();
    mtmp.emplace_front(std::forward<Args>(args)...);
    auto& bucket = buckets[_table.hash()(mtmp.front()) & (buckets.size() - 1)];
    bucket.by_owner().splice_front(mtmp, mtmp.begin());
    // total_elements isn't rcu-safe anyway, so we can update it in place
}

template <typename T, typename Hash, typename Compare>
void rcu_hashtable<T, Hash, Compare>::mutable_table::erase(iterator i)
{
    i._which_bucket->by_owner().erase(i._in_bucket);
    decrease_size_maybe_contract();
}

template <typename T, typename Hash, typename Compare>
inline
void rcu_hashtable<T, Hash, Compare>::mutable_table::increase_size_maybe_expand()
{
    bucket_array_type& buckets = *_table.ptr().read_by_owner();
    ++buckets.total_elements;
    if (buckets.total_elements < buckets.size() * 2) {
        return;
    }
    rebuild(std::max(size_t(1), buckets.size() * 2));
}

template <typename T, typename Hash, typename Compare>
inline
void rcu_hashtable<T, Hash, Compare>::mutable_table::decrease_size_maybe_contract()
{
    bucket_array_type& buckets = *_table.ptr().read_by_owner();
    --buckets.total_elements;
    if (2 * buckets.total_elements + 1 >= buckets.size()) {
        return;
    }
    rebuild(buckets.size() / 2);
}

template <typename T, typename Hash, typename Compare>
void rcu_hashtable<T, Hash, Compare>::mutable_table::rebuild(size_t new_size)
{
    assert(new_size != 0);
    bucket_array_type& buckets = *_table.ptr().read_by_owner();
    std::unique_ptr<bucket_array_type> n{new bucket_array_type(new_size)};
    n->total_elements = buckets.total_elements;
    for (auto& b : buckets) {
        for (auto& e : b.by_owner()) {
            (*n)[_table.hash()(e) & (new_size - 1)].by_owner().push_front(e);
        }
    }
    _table.ptr().assign(n.release());
}


template <typename T, typename Hash, typename Compare>
inline
auto rcu_hashtable<T, Hash, Compare>::by_owner() -> mutable_table
{
    return mutable_table{*this};
}

template <typename T, typename Hash, typename Compare>
inline
auto rcu_hashtable<T, Hash, Compare>::for_read() -> read_only_table
{
    return read_only_table{*this};
}

}

}



#endif /* RCU_HASHTABLE_HH_ */
