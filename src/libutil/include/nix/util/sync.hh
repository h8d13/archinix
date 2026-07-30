#pragma once
///@file

#include <cstdlib>
#include <mutex>
#include <condition_variable>
#include <cassert>

namespace nix {

/**
 * Ensures synchronized access to a value of type T. Used as follows:
 *
 *   struct Data { int x; ... };
 *
 *   Sync<Data> data;
 *
 *   {
 *     auto data_(data.lock());
 *     data_->x = 123;
 *   }
 *
 * Here, "data" is automatically unlocked when "data_" goes out of
 * scope.
 *
 * Upstream parameterised this over the mutex and lock types so a
 * `SharedSync` could hand out shared read locks. Nothing here ever
 * took a read lock, so there is one mutex type and `lock()` is the
 * only way in.
 */
template<class T>
class Sync
{
private:
    std::mutex mutex;
    T data;

public:

    Sync() {}

    Sync(const T & data)
        : data(data)
    {
    }

    Sync(T && data) noexcept
        : data(std::move(data))
    {
    }

    template<typename... Ts>
    Sync(Ts &&... args)
        requires requires { T{std::forward<Ts>(args)...}; }
        : data(std::forward<Ts>(args)...)
    {
    }

    Sync(Sync && other) noexcept
        : data(std::move(*other.lock()))
    {
    }

    class Lock
    {
    private:
        Sync * s;
        std::unique_lock<std::mutex> lk;
        friend Sync;

        Lock(Sync * s)
            : s(s)
            , lk(s->mutex)
        {
        }
    public:
        Lock(Lock && l) = delete;
        Lock(const Lock & l) = delete;
        Lock & operator=(Lock && l) = delete;
        Lock & operator=(const Lock & l) = delete;

        ~Lock() {}

        void wait(std::condition_variable & cv)
        {
            assert(s);
            cv.wait(lk);
        }

        template<class Predicate>
        void wait(std::condition_variable & cv, Predicate pred)
        {
            assert(s);
            cv.wait(lk, std::move(pred));
        }

        T * operator->()
        {
            return &s->data;
        }

        T & operator*()
        {
            return s->data;
        }
    };

    /**
     * Acquire exclusive access to the inner value.
     */
    Lock lock()
    {
        return Lock(this);
    }
};

} // namespace nix
